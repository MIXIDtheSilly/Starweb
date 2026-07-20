#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;

// A finished fetch waiting to be handed back to Lua on the render thread.
struct FetchDone {
    int ref = 0;  // registry ref of the page's callback
    bool ok = false;
    std::string error;
    int status = 0;
    std::string status_text;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool secure = false;
};

// Shared by the engine and every worker thread it spawned. Workers only ever touch
// this, never the lua_State, so an engine that dies mid-flight just sets `cancelled`
// and drops its reference; the last worker out frees it.
struct FetchInbox {
    std::mutex m;
    std::vector<FetchDone> done;
    int inflight = 0;
    bool cancelled = false;
    std::function<void()> wake;  // set before any worker starts
};

void install_fetch_api(lua_State* L);
void push_fetch_response(lua_State* L, const FetchDone& d);
