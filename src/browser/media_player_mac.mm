#ifdef __APPLE__
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <TargetConditionals.h>
#import <OpenGL/gl.h>
#import <OpenGL/glext.h>
#include "media_player.hpp"
#include "media_source.hpp"
#include "imgui.h"
#include <algorithm>
#include <memory>
#include <vector>
#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

// AVFoundation only asks the delegate for bytes when the URL scheme is unknown, so
// streamed assets get a private scheme and every read goes through MediaSource.
static NSString* const kStwpStreamScheme = @"stwp-stream";

static NSString* uti_for_content_type(const std::string& ct) {
    if (ct.rfind("video/quicktime", 0) == 0) return @"com.apple.quicktime-movie";
    if (ct.rfind("audio/mpeg", 0) == 0) return @"public.mp3";
    if (ct.rfind("audio/", 0) == 0) return @"public.audio";
    return @"public.mpeg-4";
}

@interface StwpResourceLoader : NSObject <AVAssetResourceLoaderDelegate>
// Shared, not borrowed: a feed block can still be parked in read_at at teardown, and
// its own reference keeps the source alive so invalidate/cancel is not a UAF.
- (instancetype)initWithSource:(std::shared_ptr<MediaSource>)source;
- (void)invalidate;
// Played fraction, or negative before a duration exists. Set each frame; the feed
// loop reads it to pace itself.
@property (nonatomic, assign) double playedFraction;
@end

@implementation StwpResourceLoader {
    std::shared_ptr<MediaSource> _source;
    dispatch_queue_t _queue;
    NSMutableSet* _cancelled;
    NSLock* _lock;
    BOOL _invalidated;
}

- (instancetype)initWithSource:(std::shared_ptr<MediaSource>)source {
    self = [super init];
    if (self) {
        _source = std::move(source);
        _playedFraction = -1.0;
        // Concurrent so a content-information request never queues behind a data
        // request that is blocked waiting on its chunks.
        _queue = dispatch_queue_create("starweb.stwp.resourceloader",
                                       DISPATCH_QUEUE_CONCURRENT);
        _cancelled = [NSMutableSet set];
        _lock = [[NSLock alloc] init];
        _invalidated = NO;
    }
    return self;
}

- (void)invalidate {
    [_lock lock];
    _invalidated = YES;
    [_lock unlock];
}

- (BOOL)isCancelled:(AVAssetResourceLoadingRequest*)request {
    [_lock lock];
    BOOL c = _invalidated || [_cancelled containsObject:[NSValue valueWithNonretainedObject:request]];
    [_lock unlock];
    return c;
}

- (BOOL)resourceLoader:(AVAssetResourceLoader*)resourceLoader
    shouldWaitForLoadingOfRequestedResource:(AVAssetResourceLoadingRequest*)loadingRequest {
    dispatch_async(_queue, ^{
        if ([self isCancelled:loadingRequest]) return;

        if (loadingRequest.contentInformationRequest) {
            AVAssetResourceLoadingContentInformationRequest* info =
                loadingRequest.contentInformationRequest;
            info.contentType = uti_for_content_type(self->_source->content_type());
            info.contentLength = self->_source->size();
            info.byteRangeAccessSupported = YES;
        }

        AVAssetResourceLoadingDataRequest* dataRequest = loadingRequest.dataRequest;
        if (!dataRequest) {
            [loadingRequest finishLoading];
            return;
        }

        int64_t offset = dataRequest.currentOffset;
        int64_t wanted = dataRequest.requestedLength -
                         (dataRequest.currentOffset - dataRequest.requestedOffset);
        if (dataRequest.requestsAllDataToEndOfResource) {
            wanted = self->_source->size() - offset;
        }

        // Sliced so a cancelled seek stops promptly and parsing can begin early.
        const int64_t kSlice = 256 * 1024;
        // AVFoundation requests everything to EOF, so throttle the lead or the loop
        // pulls the whole video regardless of how little gets watched. An unfinished
        // request just waits for the next slice.
        const int64_t kAheadBytes = 32 * 1024 * 1024;
        const int64_t total = self->_source->size();

        const int64_t startOffset = offset;
        std::vector<uint8_t> buf((size_t)std::min<int64_t>(wanted, kSlice));

        // Byte position the lead is measured against, mapped from the playhead by
        // average bitrate (coarse, but only has to bound the lead). Before a duration
        // exists, fall back to this request's start so the throttle still applies.
        auto behindOffset = [&]() -> int64_t {
            double played = self.playedFraction;
            if (played >= 0.0 && total > 0) return (int64_t)(played * (double)total);
            return startOffset;
        };

        while (wanted > 0) {
            if ([self isCancelled:loadingRequest]) return;

            while (offset - behindOffset() > kAheadBytes) {
                if ([self isCancelled:loadingRequest]) return;
                [NSThread sleepForTimeInterval:0.25];
            }

            int64_t take = std::min<int64_t>(wanted, kSlice);
            int64_t got = self->_source->read_at(offset, buf.data(), take);
            if (got <= 0) {
                [loadingRequest finishLoadingWithError:
                    [NSError errorWithDomain:NSURLErrorDomain
                                        code:NSURLErrorCannotLoadFromNetwork
                                    userInfo:nil]];
                return;
            }
            [dataRequest respondWithData:[NSData dataWithBytes:buf.data()
                                                        length:(NSUInteger)got]];
            offset += got;
            wanted -= got;
        }

        if (![self isCancelled:loadingRequest]) [loadingRequest finishLoading];
    });
    return YES;
}

- (void)resourceLoader:(AVAssetResourceLoader*)resourceLoader
    didCancelLoadingRequest:(AVAssetResourceLoadingRequest*)loadingRequest {
    [_lock lock];
    [_cancelled addObject:[NSValue valueWithNonretainedObject:loadingRequest]];
    [_lock unlock];
}

@end

@interface ObjCVideoPlayer : NSObject
@property (nonatomic, strong) AVPlayer* player;
@property (nonatomic, strong) AVPlayerItemVideoOutput* videoOutput;
@property (nonatomic, assign) GLuint textureId;
@property (nonatomic, assign) int width;
@property (nonatomic, assign) int height;
@property (nonatomic, assign) BOOL isPlaying;
@property (nonatomic, assign) double duration;
@property (nonatomic, assign) double currentTime;
@property (nonatomic, assign) float volume;
@property (nonatomic, assign) BOOL loop;
@property (nonatomic, assign) BOOL muted;
@property (nonatomic, assign) BOOL isAudioOnly;
@property (nonatomic, assign) BOOL failed;
@property (nonatomic, strong) StwpResourceLoader* loader;

- (instancetype)initWithPath:(NSString*)path audioOnly:(BOOL)audioOnly;
- (instancetype)initStreamingFrom:(int)tabId url:(NSString*)url audioOnly:(BOOL)audioOnly;
- (std::vector<std::pair<double, double>>)bufferedSpans:(double)minGap;
- (BOOL)hasError;
- (void)play;
- (void)pause;
- (void)update;
- (void)seek:(double)seconds;
- (void)setVolume:(float)vol;
- (void)setMuted:(BOOL)mute;
- (void)setLoop:(BOOL)lp;
@end

@implementation ObjCVideoPlayer {
    // Shared with the resource loader, which may still be feeding a request when
    // this player goes away.
    std::shared_ptr<MediaSource> _source;  // streaming only
}

- (std::vector<std::pair<double, double>>)bufferedSpans:(double)minGap {
    if (!_source) return {{0.0, 1.0}};
    return _source->buffered_spans(minGap);
}

- (BOOL)hasError {
    // A streamed source probes asynchronously, so failure can appear after construction.
    return _failed || (_source && _source->failed());
}

- (void)resetFields:(BOOL)audioOnly {
    _isAudioOnly = audioOnly;
    _volume = 1.0f;
    _isPlaying = false;
    _duration = 0.0;
    _currentTime = 0.0;
    _loop = false;
    _muted = false;
    _width = 0;
    _height = 0;
    _textureId = 0;
    _failed = NO;
}

// Shared tail of both initialisers: player, video output, and the sampled texture.
- (void)attachToAsset:(AVAsset*)asset {
    _player = [[AVPlayer alloc] initWithPlayerItem:[AVPlayerItem playerItemWithAsset:asset]];
    // YES for streams: with NO the player stalls on a seek into un-cached bytes and
    // never restarts. A local file never starves, so it can start immediately.
    _player.automaticallyWaitsToMinimizeStalling = _source ? YES : NO;

    if (!_isAudioOnly) {
        NSDictionary* settings = @{
            (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)
        };
        _videoOutput = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:settings];
        [_player.currentItem addOutput:_videoOutput];

        glGenTextures(1, &_textureId);
        glBindTexture(GL_TEXTURE_2D, _textureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
}

- (instancetype)initStreamingFrom:(int)tabId url:(NSString*)url audioOnly:(BOOL)audioOnly {
    self = [super init];
    if (self) {
        [self resetFields:audioOnly];

        // Non-blocking: the probe runs on the source's worker; failure surfaces
        // through hasError once it resolves.
        _source = std::make_shared<MediaSource>(tabId, std::string([url UTF8String]));
        _source->start();

        // The scheme is unknown so AVFoundation routes reads to the delegate, which
        // is also where the demuxer gets decided (content information request).
        NSURL* streamURL = [NSURL URLWithString:
            [NSString stringWithFormat:@"%@://starweb/media", kStwpStreamScheme]];
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:streamURL options:nil];

        _loader = [[StwpResourceLoader alloc] initWithSource:_source];
        [asset.resourceLoader setDelegate:_loader
                                    queue:dispatch_queue_create("starweb.stwp.loaderq", NULL)];

        // isPlayable would block on the network; let decodability surface as missing
        // frames instead.
        [self attachToAsset:asset];
    }
    return self;
}

- (instancetype)initWithPath:(NSString*)path audioOnly:(BOOL)audioOnly {
    self = [super init];
    if (self) {
        [self resetFields:audioOnly];

        NSURL* url = [NSURL fileURLWithPath:path];
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];

        // A container can parse yet not decode (e.g. H.264 4:4:4: duration and audio
        // fine, video never yields a frame). Catch it now instead of spinning.
        _failed = !asset.isPlayable;
        if (!_failed && !audioOnly) {
            AVAssetTrack* video = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
            if (video && !video.isDecodable) _failed = YES;
        }
        if (_failed) {
            NSLog(@"[media] cannot decode %@", path);
        }

        [self attachToAsset:asset];
    }
    return self;
}

- (void)dealloc {
    [_player pause];
    // Unblock anything parked in MediaSource. In-flight blocks keep the source alive
    // through their own reference, so dropping ours is safe mid-read.
    [_loader invalidate];
    if (_source) _source->cancel();
    _player = nil;
    _videoOutput = nil;
    _loader = nil;
    _source.reset();

    if (_textureId != 0) {
        glDeleteTextures(1, &_textureId);
    }
}

- (void)play {
    // AVPlayer won't play from the end, so restart from 0 instead of no-opping.
    if (_duration > 0.0 && _currentTime >= _duration - 0.1) {
        [self seek:0.0];
    }
    [_player play];
    _isPlaying = true;
}

- (void)pause {
    [_player pause];
    _isPlaying = false;
}

- (void)seek:(double)seconds {
    CMTime targetTime = CMTimeMakeWithSeconds(seconds, 600);
    // On a stream, half a second of slack lets the player settle on a keyframe it may
    // already hold instead of pulling exact-sample bytes every frame of a scrub.
    // Local files stay exact.
    CMTime tol = _source ? CMTimeMakeWithSeconds(0.5, 600) : kCMTimeZero;

    __weak ObjCVideoPlayer* weakSelf = self;
    [_player seekToTime:targetTime
        toleranceBefore:tol
         toleranceAfter:tol
      completionHandler:^(BOOL finished) {
        // A seek into un-arrived bytes leaves the rate at zero; ask to play again.
        ObjCVideoPlayer* strongSelf = weakSelf;
        if (finished && strongSelf && strongSelf.isPlaying) [strongSelf.player play];
    }];
}

- (void)setVolume:(float)vol {
    _volume = vol;
    _player.volume = _muted ? 0.0f : _volume;
}

- (void)setMuted:(BOOL)mute {
    _muted = mute;
    _player.volume = _muted ? 0.0f : _volume;
}

- (void)setLoop:(BOOL)lp {
    _loop = lp;
}

- (void)update {
    if (!_player) return;
    
    CMTime time = _player.currentTime;
    _currentTime = CMTimeGetSeconds(time);
    if (isnan(_currentTime) || isinf(_currentTime)) {
        _currentTime = 0.0;
    }
    
    AVPlayerItem* item = _player.currentItem;
    if (item) {
        CMTime dur = item.duration;
        if (CMTIME_IS_VALID(dur) && !CMTIME_IS_INDEFINITE(dur)) {
            _duration = CMTimeGetSeconds(dur);
        }
        
        // Paces the loader; negative until a duration exists so header/moov reads
        // stay unthrottled.
        if (_loader) {
            _loader.playedFraction =
                _duration > 0.0 ? std::clamp(_currentTime / _duration, 0.0, 1.0) : -1.0;
        }

        if (_loop && _currentTime >= _duration - 0.1 && _duration > 0.0) {
            [self seek:0.0];
            [_player play];
        }
        
        if (!_loop && _currentTime >= _duration && _duration > 0.0) {
            _isPlaying = false;
        }

        // Backstop for a stall the seek handler missed: UI says playing, player is
        // stopped rather than waiting for data.
        if (_isPlaying && _source &&
            _player.timeControlStatus == AVPlayerTimeControlStatusPaused &&
            !(_duration > 0.0 && _currentTime >= _duration - 0.1)) {
            [_player play];
        }
    }
    
    if (_isAudioOnly || !_videoOutput) return;
    
    CMTime itemTime = [_videoOutput itemTimeForHostTime:CACurrentMediaTime()];
    if (!CMTIME_IS_VALID(itemTime)) {
        itemTime = _player.currentTime;
    }
    
    if ([_videoOutput hasNewPixelBufferForItemTime:itemTime]) {
        CVPixelBufferRef pixelBuffer = [_videoOutput copyPixelBufferForItemTime:itemTime itemTimeForDisplay:NULL];
        if (pixelBuffer) {
            CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
            
            int w = (int)CVPixelBufferGetWidth(pixelBuffer);
            int h = (int)CVPixelBufferGetHeight(pixelBuffer);
            void* baseAddress = CVPixelBufferGetBaseAddress(pixelBuffer);
            
            glBindTexture(GL_TEXTURE_2D, _textureId);
            if (_width != w || _height != h) {
                _width = w;
                _height = h;
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, _width, _height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, baseAddress);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, _width, _height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, baseAddress);
            }
            
            CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
            CVPixelBufferRelease(pixelBuffer);
        }
    }
}
@end

// C++ Class wrapper implementations
VideoPlayer::VideoPlayer(const std::string& filepath, bool audio_only) {
    impl_ = (__bridge_retained void*)[[ObjCVideoPlayer alloc] initWithPath:[NSString stringWithUTF8String:filepath.c_str()] audioOnly:audio_only];
}

VideoPlayer::~VideoPlayer() {
    ObjCVideoPlayer* player = (__bridge_transfer ObjCVideoPlayer*)impl_;
    (void)player;
}

void VideoPlayer::play() {
    [(__bridge ObjCVideoPlayer*)impl_ play];
}

void VideoPlayer::pause() {
    [(__bridge ObjCVideoPlayer*)impl_ pause];
}

void VideoPlayer::seek(double seconds) {
    [(__bridge ObjCVideoPlayer*)impl_ seek:seconds];
}

void VideoPlayer::set_volume(float vol) {
    [(__bridge ObjCVideoPlayer*)impl_ setVolume:vol];
}

void VideoPlayer::set_muted(bool mute) {
    [(__bridge ObjCVideoPlayer*)impl_ setMuted:mute];
}

void VideoPlayer::set_loop(bool lp) {
    [(__bridge ObjCVideoPlayer*)impl_ setLoop:lp];
}

void VideoPlayer::update() {
    [(__bridge ObjCVideoPlayer*)impl_ update];
}

VideoPlayer::VideoPlayer(int tab_id, const std::string& url, bool audio_only) {
    impl_ = (__bridge_retained void*)[[ObjCVideoPlayer alloc]
        initStreamingFrom:tab_id
                      url:[NSString stringWithUTF8String:url.c_str()]
                audioOnly:audio_only];
}

bool VideoPlayer::has_error() const {
    return [(__bridge ObjCVideoPlayer*)impl_ hasError];
}

std::vector<std::pair<double, double>> VideoPlayer::buffered_spans(double min_gap) const {
    return [(__bridge ObjCVideoPlayer*)impl_ bufferedSpans:min_gap];
}

bool VideoPlayer::is_playing() const {
    return ((__bridge ObjCVideoPlayer*)impl_).isPlaying;
}

double VideoPlayer::get_current_time() const {
    return ((__bridge ObjCVideoPlayer*)impl_).currentTime;
}

double VideoPlayer::get_duration() const {
    return ((__bridge ObjCVideoPlayer*)impl_).duration;
}

float VideoPlayer::get_volume() const {
    return ((__bridge ObjCVideoPlayer*)impl_).volume;
}

bool VideoPlayer::is_muted() const {
    return ((__bridge ObjCVideoPlayer*)impl_).muted;
}

bool VideoPlayer::is_audio_only() const {
    return ((__bridge ObjCVideoPlayer*)impl_).isAudioOnly;
}

bool VideoPlayer::is_looping() const {
    return ((__bridge ObjCVideoPlayer*)impl_).loop;
}

unsigned int VideoPlayer::get_texture_id() const {
    return ((__bridge ObjCVideoPlayer*)impl_).textureId;
}

int VideoPlayer::get_width() const {
    return ((__bridge ObjCVideoPlayer*)impl_).width;
}

int VideoPlayer::get_height() const {
    return ((__bridge ObjCVideoPlayer*)impl_).height;
}

// Native file open dialog for <input type="file">. Returns the chosen path,
// or an empty string if cancelled.
#import <AppKit/AppKit.h>
#include <string>
std::string PlatformOpenFileDialog() {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];
        if ([panel runModal] == NSModalResponseOK) {
            NSURL* url = [[panel URLs] firstObject];
            if (url) return std::string([[url path] UTF8String]);
        }
    }
    return std::string();
}
#endif
