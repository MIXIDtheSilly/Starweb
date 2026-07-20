#pragma once
#include "types.hpp"
#include <string>
#include <functional>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

struct lua_State;
struct lua_Debug;
struct FetchInbox;

class ScriptEngine;
ScriptEngine* engine_from_lua(lua_State* L);

// Per-tab sandboxed Lua 5.4 interpreter for untrusted page scripts.
class ScriptEngine {
public:
    using LogSink = std::function<void(const std::string&)>;
    using AlertSink = std::function<void(const std::string&)>;
    using DomProvider = std::function<DomNode*()>;
    using NavSink = std::function<void(const std::string&)>;
    using UrlProvider = std::function<std::string()>;
    // Called from a fetch worker thread to pull the render loop out of its idle wait.
    using WakeSink = std::function<void()>;

    struct MemState {
        std::size_t used = 0;
        std::size_t cap = 0;
    };

    explicit ScriptEngine(LogSink log = {}, AlertSink alert = {});
    ~ScriptEngine();

    ScriptEngine(const ScriptEngine&) = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;
    ScriptEngine(ScriptEngine&&) = delete;
    ScriptEngine& operator=(ScriptEngine&&) = delete;

    bool run(const std::string& source, const std::string& chunk_name, std::string& error_out);

    void log(const std::string& msg);
    void alert(const std::string& msg);

    void set_dom_provider(DomProvider p) { dom_root_ = std::move(p); }
    DomNode* dom_root() { return dom_root_ ? dom_root_() : nullptr; }

    void bind_inline_handlers();
    void add_click_handler(uint64_t node_id, int ref) { click_handlers_[node_id].push_back(ref); }
    void dispatch_click(uint64_t node_id);

    void add_key_handler(bool down, int ref);
    void dispatch_key(bool down, const std::string& key);
    bool wants_keys() const { return !keydown_handlers_.empty() || !keyup_handlers_.empty(); }

    void set_nav(NavSink n) { nav_ = std::move(n); }
    void set_url_provider(UrlProvider u) { url_ = std::move(u); }
    void navigate(const std::string& url) { if (nav_) nav_(url); }
    std::string current_url() { return url_ ? url_() : std::string(); }

    int  add_timer(int ref, double delay_ms, bool repeat);
    void clear_timer(int id);
    void poll_timers();

    void set_wake(WakeSink w);
    const std::shared_ptr<FetchInbox>& fetch_inbox() const { return fetch_inbox_; }
    void poll_fetches();

    struct CanvasState {
        ImVec4 fill = ImVec4(0, 0, 0, 1);
        ImVec4 stroke = ImVec4(0, 0, 0, 1);
        float line_width = 1.0f;
        float w = 0, h = 0;
        std::vector<std::vector<ImVec2>> path;
    };
    std::vector<CanvasOp>& canvas_ops(uint64_t id) { return canvas_ops_[id]; }
    CanvasState& canvas_state(uint64_t id) { return canvas_state_[id]; }
    void set_canvas_size(uint64_t id, float w, float h) {
        auto& s = canvas_state_[id]; s.w = w; s.h = h;
    }
    const std::vector<CanvasOp>* canvas_ops_ptr(uint64_t id) const {
        auto it = canvas_ops_.find(id);
        return it == canvas_ops_.end() ? nullptr : &it->second;
    }

    int  add_raf(int ref);
    void cancel_raf(int id);
    void run_raf();

    // When this engine next needs a frame, or nullopt if nothing is scheduled.
    // A pending rAF reports "now" since it runs on the next rendered frame.
    std::optional<std::chrono::steady_clock::time_point> next_wake() const;

    bool ok() const { return L_ != nullptr; }

private:
    // Calls one registry-ref'd handler with args pushed by `push_args`. Both the
    // pushing and the call happen inside a single pcall, because pushing allocates
    // and an error raised outside one would hit Lua's panic handler and abort.
    void call_handler(int ref, int nargs, const std::function<void(lua_State*)>& push_args,
                      const char* tag);

    static int  p_install(lua_State* L);
    static int  l_print(lua_State* L);
    static int  l_alert(lua_State* L);
    static void l_hook(lua_State* L, lua_Debug* ar);

    lua_State*  L_ = nullptr;
    MemState    mem_;
    LogSink     log_;
    AlertSink   alert_;
    DomProvider dom_root_;
    NavSink     nav_;
    UrlProvider url_;
    std::unordered_map<uint64_t, std::vector<int>> click_handlers_;
    std::vector<int> keydown_handlers_;
    std::vector<int> keyup_handlers_;
    static constexpr size_t kMaxKeyHandlers = 64;

    struct Timer {
        int id;
        int ref;
        std::chrono::steady_clock::time_point due;
        double interval_ms;
        bool repeat;
    };
    std::vector<Timer> timers_;
    int next_timer_id_ = 1;
    static constexpr size_t kMaxTimers = 256;

    std::shared_ptr<FetchInbox> fetch_inbox_;

    std::unordered_map<uint64_t, std::vector<CanvasOp>> canvas_ops_;
    std::unordered_map<uint64_t, CanvasState> canvas_state_;
    std::vector<std::pair<int, int>> raf_;
    int next_raf_id_ = 1;
    static constexpr size_t kMaxRaf = 256;
    static constexpr size_t kMaxCanvasOps = 200000;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point deadline_{};

    std::size_t              mem_cap_bytes_ = 64u * 1024u * 1024u;
    std::chrono::milliseconds time_budget_  = std::chrono::milliseconds(2000);
    std::size_t              max_source_bytes_ = 4u * 1024u * 1024u;
};
