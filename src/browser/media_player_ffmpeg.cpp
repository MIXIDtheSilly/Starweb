// Windows/Linux media backend (macOS uses media_player_mac.mm). FFmpeg decodes
// on a worker thread; miniaudio plays the audio, which drives the master clock.
// STWP_FORCE_FFMPEG force-builds this on macOS to compile-check it.
#if !defined(__APPLE__) || defined(STWP_FORCE_FFMPEG)

#include "media_player.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_DECODING
#include "../thirdparty/miniaudio.h"

// FFmpeg 5.1+ swapped channel_layout for the AVChannelLayout struct; support both.
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
#define STWP_HAVE_CH_LAYOUT 1
#else
#define STWP_HAVE_CH_LAYOUT 0
#endif

namespace {

constexpr size_t kMaxVideoFrames = 30;
constexpr double kAudioQueueSeconds = 2.0;
constexpr int kOutChannels = 2;

struct DecodedVideoFrame {
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    double pts = 0.0;  // seconds
};

} // namespace

struct FFVideoPlayer {
    explicit FFVideoPlayer(const std::string& path, bool audio_only)
        : audio_only_(audio_only) {
        if (!open_input(path)) return;  // valid-but-empty; UI keeps showing a spinner
        if (!audio_only_ && video_stream_ >= 0) {
            glGenTextures(1, &texture_);
            glBindTexture(GL_TEXTURE_2D, texture_);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        if (audio_stream_ >= 0) {
            init_audio_device();
        }
        decode_thread_ = std::thread([this] { decode_loop(); });
    }

    ~FFVideoPlayer() {
        quit_.store(true);
        if (decode_thread_.joinable()) decode_thread_.join();
        if (audio_device_ready_) {
            ma_device_uninit(&audio_device_);
        }
        if (texture_ != 0) glDeleteTextures(1, &texture_);
        if (sws_) sws_freeContext(sws_);
        if (swr_) swr_free(&swr_);
        if (vdec_) avcodec_free_context(&vdec_);
        if (adec_) avcodec_free_context(&adec_);
        if (fmt_) avformat_close_input(&fmt_);
    }

    void play() {
        if (ended_.load()) {
            request_seek(0.0);
            ended_.store(false);
        }
        if (audio_stream_ < 0) {
            std::lock_guard<std::mutex> lk(clock_mutex_);
            wall_resume_ = std::chrono::steady_clock::now();
        }
        playing_.store(true);
    }

    void pause() {
        if (audio_stream_ < 0) {
            std::lock_guard<std::mutex> lk(clock_mutex_);
            wall_base_ += elapsed_since_resume_locked();
            wall_resume_ = {};
        }
        playing_.store(false);
    }

    void seek(double seconds) {
        if (seconds < 0.0) seconds = 0.0;
        if (duration_ > 0.0 && seconds > duration_) seconds = duration_;
        ended_.store(false);
        request_seek(seconds);
    }

    void set_volume(float v) { volume_.store(v); }
    void set_muted(bool m) { muted_.store(m); }
    void set_loop(bool l) { loop_.store(l); }

    // Runs every frame on the render thread (owns the GL context).
    void update() {
        double clock = master_clock();

        // Take the newest due frame, dropping late ones to stay in sync.
        DecodedVideoFrame due;
        bool have_frame = false;
        {
            std::lock_guard<std::mutex> lk(video_mutex_);
            while (!video_queue_.empty() && video_queue_.front().pts <= clock) {
                due = std::move(video_queue_.front());
                video_queue_.pop_front();
                have_frame = true;
            }
        }
        if (have_frame && texture_ != 0) {
            glBindTexture(GL_TEXTURE_2D, texture_);
            if (due.width != width_ || due.height != height_) {
                width_ = due.width;
                height_ = due.height;
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width_, height_, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, due.rgba.data());
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_RGBA,
                                GL_UNSIGNED_BYTE, due.rgba.data());
            }
        }

        current_time_.store(clock);

        if (demux_eof_.load() && !loop_.load()) {
            bool audio_drained;
            {
                std::lock_guard<std::mutex> lk(audio_mutex_);
                audio_drained = audio_fifo_.empty();
            }
            bool video_drained;
            {
                std::lock_guard<std::mutex> lk(video_mutex_);
                video_drained = video_queue_.empty();
            }
            if (audio_drained && video_drained) {
                ended_.store(true);
                playing_.store(false);
                current_time_.store(duration_);
            }
        }
    }

    bool is_playing() const { return playing_.load() && !ended_.load(); }
    double get_current_time() const { return current_time_.load(); }
    double get_duration() const { return duration_; }
    float get_volume() const { return volume_.load(); }
    bool is_muted() const { return muted_.load(); }
    bool is_audio_only() const { return audio_only_; }
    bool is_looping() const { return loop_.load(); }
    unsigned int get_texture_id() const { return texture_; }
    int get_width() const { return width_; }
    int get_height() const { return height_; }

private:
    bool open_input(const std::string& path) {
        if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) != 0) return false;
        if (avformat_find_stream_info(fmt_, nullptr) < 0) return false;

        if (fmt_->duration != AV_NOPTS_VALUE) {
            duration_ = (double)fmt_->duration / AV_TIME_BASE;
        }

        if (!audio_only_) {
            video_stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (video_stream_ >= 0 && !open_decoder(video_stream_, &vdec_)) {
                video_stream_ = -1;
            }
        }
        audio_stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (audio_stream_ >= 0 && !open_decoder(audio_stream_, &adec_)) {
            audio_stream_ = -1;
        }
        if (audio_stream_ >= 0 && !init_resampler()) {
            audio_stream_ = -1;
        }
        return video_stream_ >= 0 || audio_stream_ >= 0;
    }

    bool open_decoder(int stream_index, AVCodecContext** out_ctx) {
        AVStream* stream = fmt_->streams[stream_index];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) return false;
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx) return false;
        if (avcodec_parameters_to_context(ctx, stream->codecpar) < 0 ||
            avcodec_open2(ctx, codec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            return false;
        }
        *out_ctx = ctx;
        return true;
    }

    bool init_resampler() {
        out_sample_rate_ = adec_->sample_rate > 0 ? adec_->sample_rate : 44100;
#if STWP_HAVE_CH_LAYOUT
        AVChannelLayout out_layout;
        av_channel_layout_default(&out_layout, kOutChannels);
        AVChannelLayout in_layout;
        if (adec_->ch_layout.nb_channels > 0) {
            av_channel_layout_copy(&in_layout, &adec_->ch_layout);
        } else {
            av_channel_layout_default(&in_layout, 2);
        }
        int rc = swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_FLT, out_sample_rate_,
                                     &in_layout, adec_->sample_fmt, adec_->sample_rate,
                                     0, nullptr);
        av_channel_layout_uninit(&out_layout);
        av_channel_layout_uninit(&in_layout);
        if (rc < 0 || !swr_) return false;
#else
        int64_t in_layout = adec_->channel_layout;
        if (in_layout == 0) {
            in_layout = av_get_default_channel_layout(adec_->channels > 0 ? adec_->channels : 2);
        }
        swr_ = swr_alloc_set_opts(nullptr, av_get_default_channel_layout(kOutChannels),
                                  AV_SAMPLE_FMT_FLT, out_sample_rate_, in_layout,
                                  adec_->sample_fmt, adec_->sample_rate, 0, nullptr);
        if (!swr_) return false;
#endif
        return swr_init(swr_) >= 0;
    }

    void init_audio_device() {
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = kOutChannels;
        cfg.sampleRate = out_sample_rate_;
        cfg.dataCallback = &FFVideoPlayer::audio_callback_thunk;
        cfg.pUserData = this;
        if (ma_device_init(nullptr, &cfg, &audio_device_) != MA_SUCCESS) return;
        if (ma_device_start(&audio_device_) != MA_SUCCESS) {
            ma_device_uninit(&audio_device_);
            return;
        }
        audio_device_ready_ = true;
    }

    static void audio_callback_thunk(ma_device* device, void* output, const void* input,
                                     ma_uint32 frame_count) {
        (void)input;
        static_cast<FFVideoPlayer*>(device->pUserData)->audio_callback((float*)output, frame_count);
    }

    void audio_callback(float* out, ma_uint32 frame_count) {
        std::memset(out, 0, (size_t)frame_count * kOutChannels * sizeof(float));
        if (!playing_.load()) return;

        float gain = muted_.load() ? 0.0f : volume_.load();
        std::lock_guard<std::mutex> lk(audio_mutex_);
        ma_uint32 produced = 0;
        while (produced < frame_count && !audio_fifo_.empty()) {
            for (int c = 0; c < kOutChannels; ++c) {
                out[produced * kOutChannels + c] = audio_fifo_.front() * gain;
                audio_fifo_.pop_front();
            }
            ++produced;
        }
        samples_played_ += produced;
        audio_clock_.store(audio_base_pts_ + (double)samples_played_ / out_sample_rate_);
    }

    double master_clock() {
        if (audio_stream_ >= 0) return audio_clock_.load();
        std::lock_guard<std::mutex> lk(clock_mutex_);
        return wall_base_ + elapsed_since_resume_locked();
    }

    double elapsed_since_resume_locked() {
        if (!playing_.load() || wall_resume_.time_since_epoch().count() == 0) return 0.0;
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now - wall_resume_).count();
    }

    void request_seek(double seconds) {
        std::lock_guard<std::mutex> lk(seek_mutex_);
        seek_target_ = seconds;
        seek_requested_ = true;
    }

    void perform_seek_locked_target(double target) {
        int64_t ts = (int64_t)(target * AV_TIME_BASE);
        av_seek_frame(fmt_, -1, ts, AVSEEK_FLAG_BACKWARD);
        if (vdec_) avcodec_flush_buffers(vdec_);
        if (adec_) avcodec_flush_buffers(adec_);

        {
            std::lock_guard<std::mutex> lk(video_mutex_);
            video_queue_.clear();
        }
        {
            std::lock_guard<std::mutex> lk(audio_mutex_);
            audio_fifo_.clear();
            audio_base_pts_ = target;
            samples_played_ = 0;
        }
        audio_clock_.store(target);
        if (audio_stream_ < 0) {
            std::lock_guard<std::mutex> lk(clock_mutex_);
            wall_base_ = target;
            wall_resume_ = std::chrono::steady_clock::now();
        }
        demux_eof_.store(false);
    }

    bool queues_full() {
        if (video_stream_ >= 0) {
            std::lock_guard<std::mutex> lk(video_mutex_);
            if (video_queue_.size() >= kMaxVideoFrames) return true;
        }
        if (audio_stream_ >= 0) {
            std::lock_guard<std::mutex> lk(audio_mutex_);
            if (audio_fifo_.size() >= (size_t)(kAudioQueueSeconds * out_sample_rate_ * kOutChannels))
                return true;
        }
        return false;
    }

    void decode_loop() {
        AVPacket* pkt = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        if (!pkt || !frame) {
            if (pkt) av_packet_free(&pkt);
            if (frame) av_frame_free(&frame);
            return;
        }

        while (!quit_.load()) {
            {
                std::lock_guard<std::mutex> lk(seek_mutex_);
                if (seek_requested_) {
                    perform_seek_locked_target(seek_target_);
                    seek_requested_ = false;
                }
            }

            if (demux_eof_.load() || queues_full()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            int rc = av_read_frame(fmt_, pkt);
            if (rc < 0) {
                if (loop_.load()) {
                    request_seek(0.0);
                } else {
                    flush_decoders(frame);
                    demux_eof_.store(true);
                }
                continue;
            }

            if (pkt->stream_index == video_stream_ && vdec_) {
                decode_packet(vdec_, pkt, frame, /*is_video=*/true);
            } else if (pkt->stream_index == audio_stream_ && adec_) {
                decode_packet(adec_, pkt, frame, /*is_video=*/false);
            }
            av_packet_unref(pkt);
        }

        av_packet_free(&pkt);
        av_frame_free(&frame);
    }

    void flush_decoders(AVFrame* frame) {
        if (vdec_) {
            avcodec_send_packet(vdec_, nullptr);
            while (avcodec_receive_frame(vdec_, frame) == 0) {
                enqueue_video(frame);
                av_frame_unref(frame);
            }
        }
        if (adec_) {
            avcodec_send_packet(adec_, nullptr);
            while (avcodec_receive_frame(adec_, frame) == 0) {
                enqueue_audio(frame);
                av_frame_unref(frame);
            }
        }
    }

    void decode_packet(AVCodecContext* ctx, AVPacket* pkt, AVFrame* frame, bool is_video) {
        if (avcodec_send_packet(ctx, pkt) < 0) return;
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (is_video) {
                enqueue_video(frame);
            } else {
                enqueue_audio(frame);
            }
            av_frame_unref(frame);
        }
    }

    double frame_pts_seconds(int stream_index, const AVFrame* frame) {
        int64_t ts = frame->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) ts = frame->pts;
        if (ts == AV_NOPTS_VALUE) return 0.0;
        return ts * av_q2d(fmt_->streams[stream_index]->time_base);
    }

    void enqueue_video(AVFrame* frame) {
        int w = frame->width;
        int h = frame->height;
        if (w <= 0 || h <= 0) return;

        sws_ = sws_getCachedContext(sws_, w, h, (AVPixelFormat)frame->format, w, h,
                                    AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) return;

        DecodedVideoFrame out;
        out.width = w;
        out.height = h;
        out.pts = frame_pts_seconds(video_stream_, frame);
        out.rgba.resize((size_t)w * h * 4);
        uint8_t* dst[4] = {out.rgba.data(), nullptr, nullptr, nullptr};
        int dst_stride[4] = {w * 4, 0, 0, 0};
        sws_scale(sws_, frame->data, frame->linesize, 0, h, dst, dst_stride);

        std::lock_guard<std::mutex> lk(video_mutex_);
        video_queue_.push_back(std::move(out));
    }

    void enqueue_audio(AVFrame* frame) {
        int max_out = (int)av_rescale_rnd(swr_get_delay(swr_, adec_->sample_rate) + frame->nb_samples,
                                          out_sample_rate_, adec_->sample_rate, AV_ROUND_UP);
        if (max_out <= 0) return;
        std::vector<float> buf((size_t)max_out * kOutChannels);
        uint8_t* out_ptr = (uint8_t*)buf.data();
        int got = swr_convert(swr_, &out_ptr, max_out,
                              (const uint8_t**)frame->extended_data, frame->nb_samples);
        if (got <= 0) return;

        std::lock_guard<std::mutex> lk(audio_mutex_);
        // Anchor the clock to the first frame's PTS after a start or seek.
        if (audio_fifo_.empty() && samples_played_ == 0) {
            audio_base_pts_ = frame_pts_seconds(audio_stream_, frame);
        }
        audio_fifo_.insert(audio_fifo_.end(), buf.begin(), buf.begin() + (size_t)got * kOutChannels);
    }

    bool audio_only_ = false;

    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* vdec_ = nullptr;
    AVCodecContext* adec_ = nullptr;
    SwsContext* sws_ = nullptr;
    SwrContext* swr_ = nullptr;
    int video_stream_ = -1;
    int audio_stream_ = -1;
    int out_sample_rate_ = 44100;
    double duration_ = 0.0;

    GLuint texture_ = 0;  // render thread only
    int width_ = 0;
    int height_ = 0;

    // decode thread produces, render thread consumes
    std::deque<DecodedVideoFrame> video_queue_;
    std::mutex video_mutex_;

    std::deque<float> audio_fifo_;  // interleaved stereo float
    std::mutex audio_mutex_;
    double audio_base_pts_ = 0.0;
    long long samples_played_ = 0;
    ma_device audio_device_{};
    bool audio_device_ready_ = false;

    // only used when the file has no audio stream
    std::mutex clock_mutex_;
    double wall_base_ = 0.0;
    std::chrono::steady_clock::time_point wall_resume_{};

    std::atomic<bool> playing_{false};
    std::atomic<bool> quit_{false};
    std::atomic<bool> ended_{false};
    std::atomic<bool> loop_{false};
    std::atomic<bool> muted_{false};
    std::atomic<bool> demux_eof_{false};
    std::atomic<float> volume_{1.0f};
    std::atomic<double> audio_clock_{0.0};
    std::atomic<double> current_time_{0.0};

    std::mutex seek_mutex_;
    bool seek_requested_ = false;
    double seek_target_ = 0.0;

    std::thread decode_thread_;
};

static FFVideoPlayer* impl(void* p) { return static_cast<FFVideoPlayer*>(p); }

VideoPlayer::VideoPlayer(const std::string& filepath, bool audio_only) {
    impl_ = new FFVideoPlayer(filepath, audio_only);
}
VideoPlayer::~VideoPlayer() { delete impl(impl_); }

void VideoPlayer::play() { impl(impl_)->play(); }
void VideoPlayer::pause() { impl(impl_)->pause(); }
void VideoPlayer::update() { impl(impl_)->update(); }
void VideoPlayer::seek(double seconds) { impl(impl_)->seek(seconds); }
void VideoPlayer::set_volume(float vol) { impl(impl_)->set_volume(vol); }
void VideoPlayer::set_muted(bool mute) { impl(impl_)->set_muted(mute); }
void VideoPlayer::set_loop(bool lp) { impl(impl_)->set_loop(lp); }

bool VideoPlayer::is_playing() const { return impl(impl_)->is_playing(); }
double VideoPlayer::get_current_time() const { return impl(impl_)->get_current_time(); }
double VideoPlayer::get_duration() const { return impl(impl_)->get_duration(); }
float VideoPlayer::get_volume() const { return impl(impl_)->get_volume(); }
bool VideoPlayer::is_muted() const { return impl(impl_)->is_muted(); }
bool VideoPlayer::is_audio_only() const { return impl(impl_)->is_audio_only(); }
bool VideoPlayer::is_looping() const { return impl(impl_)->is_looping(); }
unsigned int VideoPlayer::get_texture_id() const { return impl(impl_)->get_texture_id(); }
int VideoPlayer::get_width() const { return impl(impl_)->get_width(); }
int VideoPlayer::get_height() const { return impl(impl_)->get_height(); }

#endif // !__APPLE__
