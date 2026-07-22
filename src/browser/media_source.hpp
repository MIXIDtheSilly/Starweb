#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// A seekable view of a remote media file that fills in as it is read. Chunks arrive
// via STWP Range requests into a sparse file in the media cache; a read blocks only
// on the chunks it touches, so a decoder can start on the first megabyte while the
// rest is still on the wire.
//
// The backing file is the normal cache path, so once every chunk is present it *is*
// the finished cache entry. A sidecar ".ranges" index records which chunks are held;
// its absence means complete.
class MediaSource {
public:
    static constexpr int64_t kChunkSize = 64 * 1024;

    MediaSource(int tab_id, std::string url);
    ~MediaSource();

    // Returns immediately; probe and fill run on the worker.
    void start();

    // Blocks until the probe answers. False if the resource is unavailable or ranges
    // are unsupported. Off the UI thread only.
    bool wait_ready() const;

    int64_t size() const;
    // As reported by the server; AVFoundation needs it to pick a demuxer.
    std::string content_type() const;
    bool complete() const;
    bool failed() const;

    // Blocks until the requested bytes are present. Returns bytes read, 0 at EOF,
    // -1 if cancelled or failed.
    int64_t read_at(int64_t offset, uint8_t* out, int64_t len);

    // Steers the prefetcher so a seek does not wait behind the sequential fill.
    void hint_seek(int64_t offset);

    // Held chunks as normalised [start, end] spans for the buffered bar; runs thinner
    // than min_gap are merged so it never draws more spans than it has pixels.
    std::vector<std::pair<double, double>> buffered_spans(double min_gap) const;

    void cancel();

private:
    bool probe_and_open();
    bool have_range(int64_t first_chunk, int64_t last_chunk) const;
    int64_t next_missing_from(int64_t chunk) const;
    int64_t next_wanted_chunk() const;
    void mark_present(int64_t chunk);
    void prefetch_loop();
    bool fetch_chunks(int64_t first_chunk, int64_t count);
    bool open_backing();
    void load_index();
    void save_index();

    int tab_id_;
    std::string url_;
    std::string path_;
    std::string index_path_;
    std::string content_type_;

    int64_t total_ = 0;
    int64_t chunk_count_ = 0;

    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::vector<bool> present_;
    int64_t present_count_ = 0;
    int64_t cursor_chunk_ = 0;
    // Ranges blocked read_ats are parked on. AVFoundation issues several loading
    // requests at once, so the prefetcher drains these before its own read-ahead
    // rather than letting one cursor starve the others.
    std::vector<std::pair<int64_t, int64_t>> waiters_;
    bool ready_ = false;
    bool failed_ = false;
    std::atomic<bool> cancel_{false};

    std::mutex file_mutex_;
    std::fstream file_;

    std::thread worker_;
};
