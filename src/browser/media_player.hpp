#pragma once
#include <string>
#include <utility>
#include <vector>

// The build links exactly one backend behind impl_:
//   macOS         -> media_player_mac.mm     (AVFoundation)
//   Windows/Linux -> media_player_ffmpeg.cpp (FFmpeg + miniaudio)
class VideoPlayer {
public:
    VideoPlayer(const std::string& filepath, bool audio_only = false);
    // Streams from a moon:// or star:// URL, decoding as the bytes arrive instead of
    // waiting for a complete local file.
    VideoPlayer(int tab_id, const std::string& url, bool audio_only = false);
    ~VideoPlayer();

    void play();
    void pause();
    void update();
    void seek(double seconds);
    
    void set_volume(float vol);
    void set_muted(bool mute);
    void set_loop(bool lp);

    // True when the file could not be opened or its video track cannot be decoded.
    // Without this the UI cannot tell "still buffering" from "will never decode".
    bool has_error() const;

    bool is_playing() const;
    double get_current_time() const;
    double get_duration() const;
    float get_volume() const;
    bool is_muted() const;
    bool is_audio_only() const;
    bool is_looping() const;

    // Normalised [start, end] spans of the asset held locally, for the buffered bar.
    // Local-file playback reports a single full span. min_gap is the caller's drawing
    // resolution as a fraction of the whole, so hairline gaps get merged away.
    std::vector<std::pair<double, double>> buffered_spans(double min_gap) const;

    unsigned int get_texture_id() const;
    int get_width() const;
    int get_height() const;

private:
    void* impl_;
};
