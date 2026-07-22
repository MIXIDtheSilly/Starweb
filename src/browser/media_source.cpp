#include "media_source.hpp"
#include "fetcher.hpp"
#include "globals.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace {
// 4 MB per request: big enough to amortise per-request overhead, small enough that a
// seek is not stuck behind a batch already in flight.
constexpr int64_t kPrefetchChunks = 64;

// 32 MB read-ahead ceiling past the cursor, so a viewer who stops early hasn't paid
// for the whole file. The window slides forward as reads advance the cursor.
constexpr int64_t kReadaheadChunks = 512;

int64_t parse_total_from_content_range(const std::string& value) {
    auto slash = value.rfind('/');
    if (slash == std::string::npos) return -1;
    try {
        return (int64_t)std::stoll(value.substr(slash + 1));
    } catch (...) {
        return -1;
    }
}
}  // namespace

MediaSource::MediaSource(int tab_id, std::string url)
    : tab_id_(tab_id), url_(std::move(url)) {
    path_ = get_cache_filepath(url_);
    index_path_ = path_ + ".ranges";
}

MediaSource::~MediaSource() {
    cancel();
    if (worker_.joinable()) worker_.join();
    if (!complete()) save_index();
}

void MediaSource::cancel() {
    cancel_.store(true);
    std::lock_guard<std::mutex> lk(mutex_);
    cv_.notify_all();
}

bool MediaSource::complete() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return chunk_count_ > 0 && present_count_ >= chunk_count_;
}

bool MediaSource::failed() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return failed_;
}

void MediaSource::start() {
    worker_ = std::thread([this] {
        const bool ok = probe_and_open();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            failed_ = !ok;
            ready_ = true;
            cv_.notify_all();
        }
        if (ok && !complete()) prefetch_loop();
    });
}

bool MediaSource::wait_ready() const {
    std::unique_lock<std::mutex> lk(mutex_);
    cv_.wait(lk, [&] { return ready_ || cancel_.load(); });
    return ready_ && !failed_;
}

int64_t MediaSource::size() const {
    if (!wait_ready()) return 0;
    return total_;
}

std::string MediaSource::content_type() const {
    if (!wait_ready()) return {};
    return content_type_;
}

bool MediaSource::probe_and_open() {
    // One byte is enough to learn the total size and confirm range support.
    RequestOptions probe;
    probe.timeout_secs = 10;
    probe.headers.push_back({"Range", "bytes=0-0"});
    FetchResult r = perform_fetch(tab_id_, url_, false, probe);
    if (!r.success || r.status_code != 206) return false;

    auto ct = r.headers.find("content-type");
    if (ct != r.headers.end()) content_type_ = ct->second;

    auto cr = r.headers.find("content-range");
    if (cr == r.headers.end()) return false;
    total_ = parse_total_from_content_range(cr->second);
    if (total_ <= 0) return false;

    chunk_count_ = (total_ + kChunkSize - 1) / kChunkSize;
    present_.assign((size_t)chunk_count_, false);
    present_count_ = 0;

    // Decided before open_backing resizes the file and erases the evidence.
    bool already_complete = false;
    {
        std::error_code ec;
        if (!std::filesystem::exists(index_path_, ec)) {
            auto on_disk = std::filesystem::file_size(path_, ec);
            already_complete = !ec && (int64_t)on_disk == total_;
        }
    }

    if (!open_backing()) return false;

    if (already_complete) {
        std::lock_guard<std::mutex> lk(mutex_);
        present_.assign((size_t)chunk_count_, true);
        present_count_ = chunk_count_;
    } else {
        load_index();
    }
    return true;
}

bool MediaSource::open_backing() {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);

    std::lock_guard<std::mutex> fl(file_mutex_);
    file_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!file_) {
        // Create it, then reopen for update.
        std::ofstream create(path_, std::ios::binary | std::ios::trunc);
        if (!create) return false;
        create.close();
        file_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
        if (!file_) return false;
    }

    // Size up front so writes land at any offset; the holes stay sparse.
    auto on_disk = std::filesystem::file_size(path_, ec);
    if (ec || (int64_t)on_disk != total_) {
        file_.close();
        std::filesystem::resize_file(path_, (uintmax_t)total_, ec);
        if (ec) return false;
        file_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
        if (!file_) return false;
    }
    return true;
}

void MediaSource::load_index() {
    std::ifstream idx(index_path_);
    if (!idx) return;

    int64_t stored_total = 0;
    size_t stored_hash = 0;
    idx >> stored_total >> stored_hash;
    if (stored_total != total_ || stored_hash != std::hash<std::string>{}(url_)) {
        return;  // stale index; refetch everything
    }

    std::lock_guard<std::mutex> lk(mutex_);
    int64_t chunk;
    while (idx >> chunk) {
        if (chunk >= 0 && chunk < chunk_count_ && !present_[(size_t)chunk]) {
            present_[(size_t)chunk] = true;
            present_count_++;
        }
    }
}

void MediaSource::save_index() {
    std::vector<int64_t> held;
    int64_t count = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        count = present_count_;
        if (chunk_count_ <= 0) return;
        held.reserve((size_t)present_count_);
        for (int64_t i = 0; i < chunk_count_; i++) {
            if (present_[(size_t)i]) held.push_back(i);
        }
    }

    std::error_code ec;
    if (count >= chunk_count_) {
        // Complete: drop the sidecar so the file reads as a normal cache entry.
        std::filesystem::remove(index_path_, ec);
        return;
    }

    std::ofstream idx(index_path_, std::ios::trunc);
    if (!idx) return;
    idx << total_ << " " << std::hash<std::string>{}(url_) << "\n";
    for (int64_t c : held) idx << c << "\n";
}

bool MediaSource::have_range(int64_t first_chunk, int64_t last_chunk) const {
    for (int64_t c = first_chunk; c <= last_chunk; c++) {
        if (c < 0 || c >= chunk_count_) return false;
        if (!present_[(size_t)c]) return false;
    }
    return true;
}

int64_t MediaSource::next_missing_from(int64_t chunk) const {
    for (int64_t c = std::max<int64_t>(0, chunk); c < chunk_count_; c++) {
        if (!present_[(size_t)c]) return c;
    }
    return -1;
}

// First missing chunk any blocked reader is parked on, else -1. Drained in
// registration order; each waiter spans at most a slice, so none monopolises the fill.
int64_t MediaSource::next_wanted_chunk() const {
    for (const auto& [first, last] : waiters_) {
        for (int64_t c = std::max<int64_t>(0, first);
             c <= last && c < chunk_count_; c++) {
            if (!present_[(size_t)c]) return c;
        }
    }
    return -1;
}

void MediaSource::mark_present(int64_t chunk) {
    if (chunk < 0 || chunk >= chunk_count_) return;
    if (!present_[(size_t)chunk]) {
        present_[(size_t)chunk] = true;
        present_count_++;
    }
}

std::vector<std::pair<double, double>> MediaSource::buffered_spans(double min_gap) const {
    std::vector<std::pair<double, double>> spans;
    std::lock_guard<std::mutex> lk(mutex_);
    if (chunk_count_ <= 0) return spans;

    const double scale = 1.0 / (double)chunk_count_;
    for (int64_t c = 0; c < chunk_count_;) {
        if (!present_[(size_t)c]) { c++; continue; }
        const int64_t run_start = c;
        while (c < chunk_count_ && present_[(size_t)c]) c++;

        const double start = (double)run_start * scale;
        const double end = (double)c * scale;
        // Swallow a gap too narrow to see.
        if (!spans.empty() && start - spans.back().second <= min_gap) {
            spans.back().second = end;
        } else {
            spans.push_back({start, end});
        }
    }
    return spans;
}

void MediaSource::hint_seek(int64_t offset) {
    std::lock_guard<std::mutex> lk(mutex_);
    cursor_chunk_ = std::clamp<int64_t>(offset / kChunkSize, 0, std::max<int64_t>(0, chunk_count_ - 1));
    cv_.notify_all();
}

int64_t MediaSource::read_at(int64_t offset, uint8_t* out, int64_t len) {
    if (!wait_ready()) return -1;
    if (total_ <= 0 || offset < 0 || offset >= total_ || len <= 0) return 0;
    len = std::min(len, total_ - offset);

    const int64_t first = offset / kChunkSize;
    const int64_t last = (offset + len - 1) / kChunkSize;

    {
        std::unique_lock<std::mutex> lk(mutex_);
        if (!have_range(first, last)) {
            waiters_.emplace_back(first, last);
            cv_.notify_all();
            cv_.wait(lk, [&] {
                return cancel_.load() || failed_ || have_range(first, last);
            });
            auto it = std::find(waiters_.begin(), waiters_.end(),
                                std::make_pair(first, last));
            if (it != waiters_.end()) waiters_.erase(it);
        }
        // Advance the cursor past this read so the prefetcher's window slides forward
        // and a sequential reader keeps its fill running without another round trip.
        const int64_t next = std::min(last + 1, chunk_count_ - 1);
        if (next != cursor_chunk_) {
            cursor_chunk_ = next;
            cv_.notify_all();
        }
        if (cancel_.load() || failed_) return -1;
    }

    std::lock_guard<std::mutex> fl(file_mutex_);
    file_.clear();
    file_.seekg((std::streamoff)offset);
    file_.read((char*)out, (std::streamsize)len);
    auto got = file_.gcount();
    file_.clear();
    return got > 0 ? (int64_t)got : -1;
}

void MediaSource::prefetch_loop() {
    int64_t since_save = 0;

    while (!cancel_.load()) {
        int64_t start_chunk = -1;
        int64_t run = 0;

        {
            std::unique_lock<std::mutex> lk(mutex_);
            if (present_count_ >= chunk_count_) break;

            // A parked reader outranks the sequential fill and ignores the window:
            // it is what the decoder is stalled on right now.
            start_chunk = next_wanted_chunk();
            if (start_chunk < 0) {
                // Forward of the cursor, within the window. Wrapping to the first hole
                // would walk the whole file when the cursor sits near the end.
                const int64_t limit = std::min(cursor_chunk_ + kReadaheadChunks, chunk_count_);
                const int64_t c = next_missing_from(cursor_chunk_);
                if (c >= 0 && c < limit) start_chunk = c;
            }

            if (start_chunk < 0) {
                // Buffered far enough ahead; idle until a read or seek moves the window.
                const int64_t before = cursor_chunk_;
                cv_.wait(lk, [&] {
                    return cancel_.load() || !waiters_.empty() || cursor_chunk_ != before;
                });
                continue;
            }

            // Stop the run at the first present chunk so a batch never re-downloads.
            while (run < kPrefetchChunks && start_chunk + run < chunk_count_ &&
                   !present_[(size_t)(start_chunk + run)]) {
                run++;
            }
        }

        if (run <= 0) continue;
        if (!fetch_chunks(start_chunk, run)) {
            if (cancel_.load()) break;
            std::lock_guard<std::mutex> lk(mutex_);
            failed_ = true;
            cv_.notify_all();
            break;
        }

        since_save += run;
        if (since_save >= 256) {
            save_index();
            since_save = 0;
        }
    }

    save_index();
}

bool MediaSource::fetch_chunks(int64_t first_chunk, int64_t count) {
    const int64_t start = first_chunk * kChunkSize;
    const int64_t end = std::min(total_, (first_chunk + count) * kChunkSize) - 1;
    if (end < start) return false;

    RequestOptions opt;
    opt.timeout_secs = 15;
    opt.headers.push_back(
        {"Range", "bytes=" + std::to_string(start) + "-" + std::to_string(end)});

    int64_t write_pos = start;
    // Chunks strictly below this are published; anything above is still mid-flight.
    int64_t published_through = first_chunk;
    opt.on_body_chunk = [&](const char* data, std::size_t n) {
        if (cancel_.load()) return false;

        // How many chunks are fully on disk once this body callback is written.
        int64_t complete_through;
        {
            std::lock_guard<std::mutex> fl(file_mutex_);
            file_.clear();
            file_.seekp((std::streamoff)write_pos);
            file_.write(data, (std::streamsize)n);
            if (!file_) return false;
            write_pos += (int64_t)n;

            complete_through = write_pos / kChunkSize;  // exclusive: chunks fully written
            // The final chunk is short; it completes once the write reaches EOF.
            if (write_pos >= total_) complete_through = chunk_count_;
            if (complete_through <= published_through) return true;  // no new chunk yet

            // Flush only when a boundary is crossed, so the reader that is about to be
            // unblocked sees the bytes on the same fstream — not once per 16 KB body.
            file_.flush();
            if (!file_) return false;
        }

        // Publish every chunk now fully on disk, so a reader parked inside this batch
        // wakes as soon as its bytes land instead of after the whole 4 MB request.
        // Otherwise playback stalls for a full batch round-trip at each boundary.
        std::lock_guard<std::mutex> lk(mutex_);
        for (int64_t c = published_through; c < complete_through; c++) mark_present(c);
        published_through = complete_through;
        cv_.notify_all();
        return true;
    };

    FetchResult r = perform_fetch(tab_id_, url_, false, opt);
    if (!r.success || r.status_code != 206) return false;

    const int64_t written = write_pos - start;
    if (written <= 0) return false;

    // Backstop: publish anything the incremental pass has not already marked (e.g. a
    // whole chunk whose last body callback and end-of-file coincided).
    std::lock_guard<std::mutex> lk(mutex_);
    const int64_t whole = written / kChunkSize;
    for (int64_t i = 0; i < whole; i++) mark_present(first_chunk + i);
    if (written % kChunkSize != 0 && start + written >= total_) {
        mark_present(first_chunk + whole);
    }
    cv_.notify_all();
    return true;
}
