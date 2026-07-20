#include "script.hpp"
#include "script_fetch.hpp"
#include "types.hpp"
#include "parser.hpp"
#include "../common/url_parser.hpp"

#include <cstdlib>
#include <cstring>
#include <cctype>
#include <vector>
#include <new>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

static void* sandbox_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* m = static_cast<ScriptEngine::MemState*>(ud);
    if (nsize == 0) {
        if (ptr) {
            m->used -= osize;
            std::free(ptr);
        }
        return nullptr;
    }
    size_t held = m->used - (ptr ? osize : 0);
    if (nsize > m->cap - held) return nullptr;
    void* np = std::realloc(ptr, nsize);
    if (!np) return nullptr;
    m->used = held + nsize;
    return np;
}

ScriptEngine* engine_from_lua(lua_State* L) {
    return *static_cast<ScriptEngine**>(lua_getextraspace(L));
}

static const char* kElementMT = "StarElement";

static DomNode* current_root(lua_State* L) {
    ScriptEngine* eng = engine_from_lua(L);
    return eng ? eng->dom_root() : nullptr;
}

static DomNode* find_by_node_id(DomNode* n, uint64_t id) {
    if (!n) return nullptr;
    if (n->node_id == id) return n;
    for (auto& c : n->children)
        if (DomNode* f = find_by_node_id(&c, id)) return f;
    return nullptr;
}

template <class Pred>
static DomNode* find_first(DomNode* n, Pred pred) {
    if (!n) return nullptr;
    if (pred(*n)) return n;
    for (auto& c : n->children)
        if (DomNode* f = find_first(&c, pred)) return f;
    return nullptr;
}

template <class Pred>
static void find_all(DomNode* n, Pred pred, std::vector<DomNode*>& out) {
    if (!n) return;
    if (pred(*n)) out.push_back(n);
    for (auto& c : n->children) find_all(&c, pred, out);
}

static bool has_class(const std::string& attr, const std::string& want) {
    size_t start = 0;
    while (start <= attr.size()) {
        size_t sp = attr.find(' ', start);
        size_t n = (sp == std::string::npos) ? attr.size() - start : sp - start;
        if (attr.compare(start, n, want) == 0) return true;
        if (sp == std::string::npos) break;
        start = sp + 1;
    }
    return false;
}

static std::string to_lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static void push_element(lua_State* L, uint64_t id) {
    void* ud = lua_newuserdatauv(L, sizeof(uint64_t), 0);
    *static_cast<uint64_t*>(ud) = id;
    luaL_setmetatable(L, kElementMT);
}

static DomNode* resolve_element(lua_State* L, int idx) {
    void* ud = luaL_checkudata(L, idx, kElementMT);
    return find_by_node_id(current_root(L), *static_cast<uint64_t*>(ud));
}

static void push_str(lua_State* L, const std::string& s) {
    lua_pushlstring(L, s.data(), s.size());
}

static const char* kStyleMT = "StarStyle";

static void push_style(lua_State* L, uint64_t id) {
    void* ud = lua_newuserdatauv(L, sizeof(uint64_t), 0);
    *static_cast<uint64_t*>(ud) = id;
    luaL_setmetatable(L, kStyleMT);
}

static std::string arg_to_string(lua_State* L, int idx) {
    size_t len = 0;
    const char* s = luaL_tolstring(L, idx, &len);
    std::string out(s, len);
    lua_pop(L, 1);
    return out;
}

static int el_setAttribute(lua_State* L) {
    DomNode* n = resolve_element(L, 1);
    std::string name = luaL_checkstring(L, 2);
    std::string val = arg_to_string(L, 3);
    if (!n) return 0;
    if      (name == "id")          n->id = val;
    else if (name == "class")       n->class_name = val;
    else if (name == "href")        n->href = val;
    else if (name == "src")         n->src = val;
    else if (name == "value")       n->value = val;
    else if (name == "placeholder") n->placeholder = val;
    else if (name == "style") {
        n->inline_style = val;
        n->has_inline_style = true;
        n->parsed_inline_style = CssStyle();
        parse_css_properties(val, n->parsed_inline_style);
    }
    return 0;
}

static int el_getAttribute(lua_State* L) {
    DomNode* n = resolve_element(L, 1);
    std::string name = luaL_checkstring(L, 2);
    if (!n) { lua_pushnil(L); return 1; }
    if      (name == "id")          push_str(L, n->id);
    else if (name == "class")       push_str(L, n->class_name);
    else if (name == "href")        push_str(L, n->href);
    else if (name == "src")         push_str(L, n->src);
    else if (name == "value")       push_str(L, n->value);
    else if (name == "placeholder") push_str(L, n->placeholder);
    else lua_pushnil(L);
    return 1;
}

static int el_addEventListener(lua_State* L) {
    DomNode* n = resolve_element(L, 1);
    std::string ev = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (!n || ev != "click") return 0;
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (ScriptEngine* eng = engine_from_lua(L)) eng->add_click_handler(n->node_id, ref);
    return 0;
}

// document.addEventListener(type, fn) — page-wide key events. Called with a dot,
// like the rest of the document table, so the event name is the first argument.
static int doc_addEventListener(lua_State* L) {
    std::string ev = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (ev != "keydown" && ev != "keyup") return 0;
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (ScriptEngine* eng = engine_from_lua(L)) eng->add_key_handler(ev == "keydown", ref);
    else luaL_unref(L, LUA_REGISTRYINDEX, ref);
    return 0;
}

static int el_getContext(lua_State* L);

static int element_index(lua_State* L) {
    DomNode* n = resolve_element(L, 1);
    const char* key = luaL_checkstring(L, 2);
    if (std::strcmp(key, "setAttribute")     == 0) { lua_pushcfunction(L, &el_setAttribute); return 1; }
    if (std::strcmp(key, "getAttribute")     == 0) { lua_pushcfunction(L, &el_getAttribute); return 1; }
    if (std::strcmp(key, "addEventListener") == 0) { lua_pushcfunction(L, &el_addEventListener); return 1; }
    if (std::strcmp(key, "getContext")       == 0) { lua_pushcfunction(L, &el_getContext); return 1; }
    if (!n) { lua_pushnil(L); return 1; }
    if (std::strcmp(key, "style") == 0) { push_style(L, n->node_id); return 1; }
    if (n->tag == "canvas" && (std::strcmp(key, "width") == 0 || std::strcmp(key, "height") == 0)) {
        ScriptEngine* eng = engine_from_lua(L);
        auto& s = eng->canvas_state(n->node_id);
        lua_pushnumber(L, key[0] == 'w' ? s.w : s.h);
        return 1;
    }
    if      (std::strcmp(key, "textContent") == 0) push_str(L, n->text_content);
    else if (std::strcmp(key, "value")       == 0) push_str(L, n->value);
    else if (std::strcmp(key, "id")          == 0) push_str(L, n->id);
    else if (std::strcmp(key, "className")   == 0) push_str(L, n->class_name);
    else if (std::strcmp(key, "tagName")     == 0) push_str(L, n->tag);
    else if (std::strcmp(key, "href")        == 0) push_str(L, n->href);
    else lua_pushnil(L);
    return 1;
}

static int element_newindex(lua_State* L) {
    DomNode* n = resolve_element(L, 1);
    const char* key = luaL_checkstring(L, 2);
    if (!n) return 0;
    std::string val = arg_to_string(L, 3);
    if (std::strcmp(key, "textContent") == 0 || std::strcmp(key, "innerText") == 0) {
        n->children.clear();
        n->text_content = val;
    } else if (std::strcmp(key, "value")     == 0) n->value = val;
    else if   (std::strcmp(key, "id")        == 0) n->id = val;
    else if   (std::strcmp(key, "className") == 0) n->class_name = val;
    return 0;
}

static int style_newindex(lua_State* L) {
    void* ud = luaL_checkudata(L, 1, kStyleMT);
    DomNode* n = find_by_node_id(current_root(L), *static_cast<uint64_t*>(ud));
    std::string prop = luaL_checkstring(L, 2);
    std::string val = arg_to_string(L, 3);
    if (!n) return 0;
    n->has_inline_style = true;
    parse_css_properties(prop + ":" + val, n->parsed_inline_style);
    return 0;
}

static int style_index(lua_State* L) {
    lua_pushnil(L);
    return 1;
}

static const char* kLocationMT = "StarLocation";

static int loc_assign(lua_State* L) {
    std::string url = luaL_checkstring(L, lua_gettop(L));
    if (ScriptEngine* eng = engine_from_lua(L)) eng->navigate(url);
    return 0;
}

static int loc_reload(lua_State* L) {
    if (ScriptEngine* eng = engine_from_lua(L)) eng->navigate(eng->current_url());
    return 0;
}

static int location_index(lua_State* L) {
    ScriptEngine* eng = engine_from_lua(L);
    const char* key = luaL_checkstring(L, 2);
    std::string url = eng ? eng->current_url() : std::string();
    if (std::strcmp(key, "assign") == 0) { lua_pushcfunction(L, &loc_assign); return 1; }
    if (std::strcmp(key, "reload") == 0) { lua_pushcfunction(L, &loc_reload); return 1; }
    if (std::strcmp(key, "href") == 0) { push_str(L, url); return 1; }
    auto p = parse_url(url);
    if      (std::strcmp(key, "protocol") == 0) push_str(L, p ? p->scheme : "");
    else if (std::strcmp(key, "host")     == 0) push_str(L, p ? p->host : "");
    else if (std::strcmp(key, "pathname") == 0) push_str(L, p ? p->path : "");
    else lua_pushnil(L);
    return 1;
}

static int location_newindex(lua_State* L) {
    const char* key = luaL_checkstring(L, 2);
    std::string val = arg_to_string(L, 3);
    if (std::strcmp(key, "href") == 0)
        if (ScriptEngine* eng = engine_from_lua(L)) eng->navigate(val);
    return 0;
}

static int location_tostring(lua_State* L) {
    ScriptEngine* eng = engine_from_lua(L);
    push_str(L, eng ? eng->current_url() : std::string());
    return 1;
}

static int l_setTimeout(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    double ms = luaL_optnumber(L, 2, 0);
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ScriptEngine* eng = engine_from_lua(L);
    lua_pushinteger(L, eng ? eng->add_timer(ref, ms, false) : 0);
    return 1;
}

static int l_setInterval(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    double ms = luaL_optnumber(L, 2, 0);
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ScriptEngine* eng = engine_from_lua(L);
    lua_pushinteger(L, eng ? eng->add_timer(ref, ms, true) : 0);
    return 1;
}

static int l_clearTimer(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    if (ScriptEngine* eng = engine_from_lua(L)) eng->clear_timer(id);
    return 0;
}

static const char* kCanvasCtxMT = "StarCanvasCtx";

static uint64_t ctx_node(lua_State* L) {
    return *static_cast<uint64_t*>(luaL_checkudata(L, 1, kCanvasCtxMT));
}

static void canvas_push(ScriptEngine* eng, uint64_t id, const CanvasOp& op) {
    auto& ops = eng->canvas_ops(id);
    if (ops.size() < 200000) ops.push_back(op);
}

static int ctx_clearRect(lua_State* L) {
    if (ScriptEngine* eng = engine_from_lua(L)) eng->canvas_ops(ctx_node(L)).clear();
    return 0;
}

static int ctx_fillRect(lua_State* L) {
    ScriptEngine* eng = engine_from_lua(L);
    if (!eng) return 0;
    uint64_t id = ctx_node(L);
    CanvasOp op;
    op.kind = CanvasOp::FillRect;
    op.a = (float)luaL_checknumber(L, 2); op.b = (float)luaL_checknumber(L, 3);
    op.c = (float)luaL_checknumber(L, 4); op.d = (float)luaL_checknumber(L, 5);
    op.color = eng->canvas_state(id).fill;
    canvas_push(eng, id, op);
    return 0;
}

static int ctx_strokeRect(lua_State* L) {
    ScriptEngine* eng = engine_from_lua(L);
    if (!eng) return 0;
    uint64_t id = ctx_node(L);
    auto& st = eng->canvas_state(id);
    CanvasOp op;
    op.kind = CanvasOp::StrokeRect;
    op.a = (float)luaL_checknumber(L, 2); op.b = (float)luaL_checknumber(L, 3);
    op.c = (float)luaL_checknumber(L, 4); op.d = (float)luaL_checknumber(L, 5);
    op.color = st.stroke; op.line_width = st.line_width;
    canvas_push(eng, id, op);
    return 0;
}

static int ctx_beginPath(lua_State* L) {
    if (ScriptEngine* eng = engine_from_lua(L)) eng->canvas_state(ctx_node(L)).path.clear();
    return 0;
}

static int ctx_moveTo(lua_State* L) {
    ScriptEngine* eng = engine_from_lua(L);
    if (!eng) return 0;
    eng->canvas_state(ctx_node(L)).path.push_back(
        { ImVec2((float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3)) });
    return 0;
}

static int ctx_lineTo(lua_State* L) {
    ScriptEngine* eng = engine_from_lua(L);
    if (!eng) return 0;
    auto& path = eng->canvas_state(ctx_node(L)).path;
    if (path.empty()) path.push_back({});
    path.back().push_back(ImVec2((float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3)));
    return 0;
}

static int ctx_stroke(lua_State* L) {
    ScriptEngine* eng = engine_from_lua(L);
    if (!eng) return 0;
    uint64_t id = ctx_node(L);
    auto& st = eng->canvas_state(id);
    for (auto& sub : st.path) {
        for (size_t i = 1; i < sub.size(); ++i) {
            CanvasOp op;
            op.kind = CanvasOp::Line;
            op.a = sub[i - 1].x; op.b = sub[i - 1].y;
            op.c = sub[i].x;     op.d = sub[i].y;
            op.color = st.stroke; op.line_width = st.line_width;
            canvas_push(eng, id, op);
        }
    }
    return 0;
}

static int ctx_fill(lua_State* L) {
    ScriptEngine* eng = engine_from_lua(L);
    if (!eng) return 0;
    uint64_t id = ctx_node(L);
    auto& st = eng->canvas_state(id);
    if (!st.path.empty() && st.path[0].size() >= 3) {
        CanvasOp op;
        op.kind = CanvasOp::PolyFill;
        op.color = st.fill;
        op.pts = st.path[0];
        canvas_push(eng, id, op);
    }
    return 0;
}

static int ctx_arc(lua_State* L) {
    ScriptEngine* eng = engine_from_lua(L);
    if (!eng) return 0;
    uint64_t id = ctx_node(L);
    auto& st = eng->canvas_state(id);
    CanvasOp op;
    op.kind = CanvasOp::Circle;
    op.a = (float)luaL_checknumber(L, 2);
    op.b = (float)luaL_checknumber(L, 3);
    op.c = (float)luaL_checknumber(L, 4);
    op.color = st.stroke; op.line_width = st.line_width;
    canvas_push(eng, id, op);
    return 0;
}

static int ctx_fillText(lua_State* L) {
    ScriptEngine* eng = engine_from_lua(L);
    if (!eng) return 0;
    uint64_t id = ctx_node(L);
    CanvasOp op;
    op.kind = CanvasOp::Text;
    op.text = luaL_checkstring(L, 2);
    op.a = (float)luaL_checknumber(L, 3);
    op.b = (float)luaL_checknumber(L, 4);
    op.color = eng->canvas_state(id).fill;
    canvas_push(eng, id, op);
    return 0;
}

static int ctx_index(lua_State* L) {
    const char* key = luaL_checkstring(L, 2);
    lua_CFunction fn = nullptr;
    if      (std::strcmp(key, "clearRect")  == 0) fn = &ctx_clearRect;
    else if (std::strcmp(key, "fillRect")   == 0) fn = &ctx_fillRect;
    else if (std::strcmp(key, "strokeRect") == 0) fn = &ctx_strokeRect;
    else if (std::strcmp(key, "beginPath")  == 0) fn = &ctx_beginPath;
    else if (std::strcmp(key, "moveTo")     == 0) fn = &ctx_moveTo;
    else if (std::strcmp(key, "lineTo")     == 0) fn = &ctx_lineTo;
    else if (std::strcmp(key, "stroke")     == 0) fn = &ctx_stroke;
    else if (std::strcmp(key, "fill")       == 0) fn = &ctx_fill;
    else if (std::strcmp(key, "arc")        == 0) fn = &ctx_arc;
    else if (std::strcmp(key, "fillText")   == 0) fn = &ctx_fillText;
    if (fn) lua_pushcfunction(L, fn); else lua_pushnil(L);
    return 1;
}

static int ctx_newindex(lua_State* L) {
    ScriptEngine* eng = engine_from_lua(L);
    if (!eng) return 0;
    uint64_t id = ctx_node(L);
    const char* key = luaL_checkstring(L, 2);
    auto& st = eng->canvas_state(id);
    if      (std::strcmp(key, "fillStyle")   == 0) st.fill = parse_color(luaL_checkstring(L, 3));
    else if (std::strcmp(key, "strokeStyle") == 0) st.stroke = parse_color(luaL_checkstring(L, 3));
    else if (std::strcmp(key, "lineWidth")   == 0) st.line_width = (float)luaL_checknumber(L, 3);
    return 0;
}

static int el_getContext(lua_State* L) {
    uint64_t id = *static_cast<uint64_t*>(luaL_checkudata(L, 1, "StarElement"));
    void* ud = lua_newuserdatauv(L, sizeof(uint64_t), 0);
    *static_cast<uint64_t*>(ud) = id;
    luaL_setmetatable(L, kCanvasCtxMT);
    return 1;
}

static int l_requestAnimationFrame(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ScriptEngine* eng = engine_from_lua(L);
    lua_pushinteger(L, eng ? eng->add_raf(ref) : 0);
    return 1;
}

static int l_cancelAnimationFrame(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    if (ScriptEngine* eng = engine_from_lua(L)) eng->cancel_raf(id);
    return 0;
}

static int doc_getElementById(lua_State* L) {
    std::string id = luaL_checkstring(L, 1);
    DomNode* f = find_first(current_root(L),
        [&](const DomNode& n) { return n.id == id; });
    if (f) push_element(L, f->node_id); else lua_pushnil(L);
    return 1;
}

static void push_element_array(lua_State* L, const std::vector<DomNode*>& v) {
    lua_createtable(L, (int)v.size(), 0);
    for (size_t i = 0; i < v.size(); ++i) {
        push_element(L, v[i]->node_id);
        lua_rawseti(L, -2, (lua_Integer)(i + 1));
    }
}

static int doc_getElementsByClassName(lua_State* L) {
    std::string cls = luaL_checkstring(L, 1);
    std::vector<DomNode*> out;
    find_all(current_root(L),
        [&](const DomNode& n) { return has_class(n.class_name, cls); }, out);
    push_element_array(L, out);
    return 1;
}

static int doc_getElementsByTagName(lua_State* L) {
    std::string tag = to_lower(luaL_checkstring(L, 1));
    std::vector<DomNode*> out;
    find_all(current_root(L),
        [&](const DomNode& n) { return n.tag == tag; }, out);
    push_element_array(L, out);
    return 1;
}

static int doc_querySelector(lua_State* L) {
    std::string sel = luaL_checkstring(L, 1);
    DomNode* f = nullptr;
    DomNode* root = current_root(L);
    if (!sel.empty() && sel[0] == '#') {
        std::string id = sel.substr(1);
        f = find_first(root, [&](const DomNode& n) { return n.id == id; });
    } else if (!sel.empty() && sel[0] == '.') {
        std::string cls = sel.substr(1);
        f = find_first(root, [&](const DomNode& n) { return has_class(n.class_name, cls); });
    } else {
        std::string tag = to_lower(sel);
        f = find_first(root, [&](const DomNode& n) { return n.tag == tag; });
    }
    if (f) push_element(L, f->node_id); else lua_pushnil(L);
    return 1;
}

void ScriptEngine::l_hook(lua_State* L, lua_Debug*) {
    ScriptEngine* eng = engine_from_lua(L);
    if (eng && std::chrono::steady_clock::now() > eng->deadline_) {
        luaL_error(L, "script aborted: exceeded time budget");
    }
}

int ScriptEngine::l_print(lua_State* L) {
    ScriptEngine* eng = engine_from_lua(L);
    std::string out;
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) out += '\t';
        out.append(s, len);
        lua_pop(L, 1);
    }
    if (eng) eng->log(out);
    return 0;
}

int ScriptEngine::l_alert(lua_State* L) {
    ScriptEngine* eng = engine_from_lua(L);
    size_t len = 0;
    const char* s = lua_gettop(L) >= 1 ? luaL_tolstring(L, 1, &len) : "";
    if (eng) eng->alert(std::string(s, len));
    return 0;
}

int ScriptEngine::p_install(lua_State* L) {
    static const luaL_Reg kSafeLibs[] = {
        {LUA_GNAME,       luaopen_base},
        {LUA_TABLIBNAME,  luaopen_table},
        {LUA_STRLIBNAME,  luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_COLIBNAME,   luaopen_coroutine},
        {LUA_UTF8LIBNAME, luaopen_utf8},
        {nullptr, nullptr},
    };
    for (const luaL_Reg* lib = kSafeLibs; lib->func; ++lib) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }

    static const char* kRemove[] = {
        "load", "loadfile", "dofile", "loadstring", "require", "collectgarbage", nullptr,
    };
    for (const char** g = kRemove; *g; ++g) {
        lua_pushnil(L);
        lua_setglobal(L, *g);
    }

    lua_getglobal(L, "string");
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);

    lua_pushcfunction(L, &ScriptEngine::l_print);
    lua_setglobal(L, "print");

    lua_pushcfunction(L, &ScriptEngine::l_alert);
    lua_setglobal(L, "alert");

    lua_newtable(L);
    lua_pushcfunction(L, &ScriptEngine::l_print);
    lua_setfield(L, -2, "log");
    lua_setglobal(L, "console");

    luaL_newmetatable(L, kElementMT);
    lua_pushcfunction(L, &element_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, &element_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    luaL_newmetatable(L, kStyleMT);
    lua_pushcfunction(L, &style_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, &style_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, &doc_getElementById);        lua_setfield(L, -2, "getElementById");
    lua_pushcfunction(L, &doc_getElementsByClassName); lua_setfield(L, -2, "getElementsByClassName");
    lua_pushcfunction(L, &doc_getElementsByTagName);   lua_setfield(L, -2, "getElementsByTagName");
    lua_pushcfunction(L, &doc_querySelector);          lua_setfield(L, -2, "querySelector");
    lua_pushcfunction(L, &doc_addEventListener);       lua_setfield(L, -2, "addEventListener");
    lua_setglobal(L, "document");

    lua_pushcfunction(L, &l_setTimeout);  lua_setglobal(L, "setTimeout");
    lua_pushcfunction(L, &l_setInterval); lua_setglobal(L, "setInterval");
    lua_pushcfunction(L, &l_clearTimer);  lua_setglobal(L, "clearTimeout");
    lua_pushcfunction(L, &l_clearTimer);  lua_setglobal(L, "clearInterval");
    lua_pushcfunction(L, &l_requestAnimationFrame); lua_setglobal(L, "requestAnimationFrame");
    lua_pushcfunction(L, &l_cancelAnimationFrame);  lua_setglobal(L, "cancelAnimationFrame");

    luaL_newmetatable(L, kCanvasCtxMT);
    lua_pushcfunction(L, &ctx_index);    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, &ctx_newindex); lua_setfield(L, -2, "__newindex");
    lua_pushboolean(L, 0);               lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    luaL_newmetatable(L, kLocationMT);
    lua_pushcfunction(L, &location_index);    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, &location_newindex); lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, &location_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushboolean(L, 0);                    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    install_fetch_api(L);

    lua_newtable(L);
    lua_newuserdatauv(L, 1, 0);
    luaL_setmetatable(L, kLocationMT);
    lua_setfield(L, -2, "location");
    lua_pushcfunction(L, &l_setTimeout);  lua_setfield(L, -2, "setTimeout");
    lua_pushcfunction(L, &l_setInterval); lua_setfield(L, -2, "setInterval");
    lua_pushcfunction(L, &l_clearTimer);  lua_setfield(L, -2, "clearTimeout");
    lua_pushcfunction(L, &l_clearTimer);  lua_setfield(L, -2, "clearInterval");
    lua_pushcfunction(L, &l_requestAnimationFrame); lua_setfield(L, -2, "requestAnimationFrame");
    lua_pushcfunction(L, &l_cancelAnimationFrame);  lua_setfield(L, -2, "cancelAnimationFrame");
    lua_pushcfunction(L, &ScriptEngine::l_alert); lua_setfield(L, -2, "alert");
    lua_getglobal(L, "fetch");                    lua_setfield(L, -2, "fetch");
    lua_pushvalue(L, -1);
    lua_setglobal(L, "window");
    lua_getfield(L, -1, "location");
    lua_setglobal(L, "location");
    lua_pop(L, 1);

    return 0;
}

ScriptEngine::ScriptEngine(LogSink log, AlertSink alert)
    : log_(std::move(log)), alert_(std::move(alert)) {
    fetch_inbox_ = std::make_shared<FetchInbox>();
    mem_.cap = mem_cap_bytes_;
    L_ = lua_newstate(&sandbox_alloc, &mem_);
    if (!L_) return;

    *static_cast<ScriptEngine**>(lua_getextraspace(L_)) = this;

    lua_pushcfunction(L_, &ScriptEngine::p_install);
    if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
        const char* msg = lua_tostring(L_, -1);
        log(std::string("[script] sandbox init failed: ") + (msg ? msg : "?"));
        lua_pop(L_, 1);
        lua_close(L_);
        L_ = nullptr;
        return;
    }

    lua_sethook(L_, &ScriptEngine::l_hook, LUA_MASKCOUNT, 1000);
}

ScriptEngine::~ScriptEngine() {
    {
        std::lock_guard<std::mutex> lk(fetch_inbox_->m);
        fetch_inbox_->cancelled = true;
        fetch_inbox_->wake = nullptr;
    }
    if (L_) lua_close(L_);
}

void ScriptEngine::set_wake(WakeSink w) {
    std::lock_guard<std::mutex> lk(fetch_inbox_->m);
    fetch_inbox_->wake = std::move(w);
}

void ScriptEngine::poll_fetches() {
    if (!L_) return;
    std::vector<FetchDone> batch;
    {
        std::lock_guard<std::mutex> lk(fetch_inbox_->m);
        batch.swap(fetch_inbox_->done);
    }
    if (batch.empty()) return;

    deadline_ = std::chrono::steady_clock::now() + time_budget_;
    for (const FetchDone& d : batch) {
        call_handler(d.ref, 2, [&d](lua_State* L) {
            if (d.ok) {
                lua_pushnil(L);
                push_fetch_response(L, d);
            } else {
                lua_pushlstring(L, d.error.data(), d.error.size());
                lua_pushnil(L);
            }
        }, "[fetch]");
        luaL_unref(L_, LUA_REGISTRYINDEX, d.ref);
    }
}

void ScriptEngine::log(const std::string& msg) {
    if (log_) log_(msg);
}

void ScriptEngine::alert(const std::string& msg) {
    if (alert_) alert_(msg);
}

bool ScriptEngine::run(const std::string& source, const std::string& chunk_name,
                       std::string& error_out) {
    if (!L_) { error_out = "script engine unavailable"; return false; }
    if (source.size() > max_source_bytes_) {
        error_out = "script rejected: source exceeds size limit";
        return false;
    }

    deadline_ = std::chrono::steady_clock::now() + time_budget_;

    std::string chunkid = "=" + chunk_name;
    int status = luaL_loadbufferx(L_, source.data(), source.size(),
                                  chunkid.c_str(), "t");  // "t" = text only, no bytecode
    if (status == LUA_OK) {
        status = lua_pcall(L_, 0, 0, 0);
    }
    if (status != LUA_OK) {
        const char* msg = lua_tostring(L_, -1);
        error_out = msg ? msg : "unknown script error";
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

void ScriptEngine::bind_inline_handlers() {
    if (!L_) return;
    std::vector<DomNode*> nodes;
    find_all(dom_root(), [](const DomNode& n) { return !n.onclick.empty(); }, nodes);
    for (DomNode* n : nodes) {
        if (luaL_loadbufferx(L_, n->onclick.data(), n->onclick.size(), "=onclick", "t") == LUA_OK) {
            add_click_handler(n->node_id, luaL_ref(L_, LUA_REGISTRYINDEX));
        } else {
            const char* msg = lua_tostring(L_, -1);
            log(std::string("[onclick] ") + (msg ? msg : "?"));
            lua_pop(L_, 1);
        }
    }
}

namespace {
struct HandlerCall {
    int ref;
    int nargs;
    const std::function<void(lua_State*)>* push_args;
};

int invoke_handler(lua_State* L) {
    auto* hc = static_cast<HandlerCall*>(lua_touserdata(L, 1));
    lua_rawgeti(L, LUA_REGISTRYINDEX, hc->ref);
    (*hc->push_args)(L);
    lua_call(L, hc->nargs, 0);
    return 0;
}
} // namespace

void ScriptEngine::call_handler(int ref, int nargs,
                                const std::function<void(lua_State*)>& push_args,
                                const char* tag) {
    if (!L_) return;
    if (!lua_checkstack(L_, nargs + 4)) {
        log(std::string(tag) + " out of stack, handler dropped");
        return;
    }
    HandlerCall hc{ref, nargs, &push_args};
    lua_pushcfunction(L_, &invoke_handler);
    lua_pushlightuserdata(L_, &hc);
    if (lua_pcall(L_, 1, 0, 0) != LUA_OK) {
        const char* msg = lua_tostring(L_, -1);
        log(std::string(tag) + " " + (msg ? msg : "?"));
        lua_pop(L_, 1);
    }
}

void ScriptEngine::dispatch_click(uint64_t node_id) {
    if (!L_) return;
    auto it = click_handlers_.find(node_id);
    if (it == click_handlers_.end()) return;
    // Copied because a handler may call addEventListener and grow this vector.
    std::vector<int> refs = it->second;
    deadline_ = std::chrono::steady_clock::now() + time_budget_;
    for (int ref : refs) {
        call_handler(ref, 1, [node_id](lua_State* L) { push_element(L, node_id); }, "[click]");
    }
}

void ScriptEngine::add_key_handler(bool down, int ref) {
    auto& list = down ? keydown_handlers_ : keyup_handlers_;
    if (!L_ || list.size() >= kMaxKeyHandlers) {
        if (L_) luaL_unref(L_, LUA_REGISTRYINDEX, ref);
        return;
    }
    list.push_back(ref);
}

void ScriptEngine::dispatch_key(bool down, const std::string& key) {
    if (!L_) return;
    std::vector<int> refs = down ? keydown_handlers_ : keyup_handlers_;
    if (refs.empty()) return;
    deadline_ = std::chrono::steady_clock::now() + time_budget_;
    const char* tag = down ? "[keydown]" : "[keyup]";
    for (int ref : refs) {
        call_handler(ref, 1, [&key](lua_State* L) {
            lua_newtable(L);
            lua_pushlstring(L, key.data(), key.size());
            lua_setfield(L, -2, "key");
        }, tag);
    }
}

int ScriptEngine::add_timer(int ref, double delay_ms, bool repeat) {
    if (!L_ || timers_.size() >= kMaxTimers) {
        if (L_) luaL_unref(L_, LUA_REGISTRYINDEX, ref);
        return 0;
    }
    if (delay_ms < 0) delay_ms = 0;
    if (repeat && delay_ms < 4) delay_ms = 4;
    int id = next_timer_id_++;
    auto due = std::chrono::steady_clock::now() +
               std::chrono::microseconds((long long)(delay_ms * 1000.0));
    timers_.push_back({id, ref, due, delay_ms, repeat});
    return id;
}

void ScriptEngine::clear_timer(int id) {
    for (size_t i = 0; i < timers_.size(); ++i) {
        if (timers_[i].id == id) {
            if (L_) luaL_unref(L_, LUA_REGISTRYINDEX, timers_[i].ref);
            timers_.erase(timers_.begin() + i);
            return;
        }
    }
}

void ScriptEngine::poll_timers() {
    if (!L_ || timers_.empty()) return;
    auto now = std::chrono::steady_clock::now();
    std::vector<int> due_ids;
    for (const Timer& t : timers_)
        if (now >= t.due) due_ids.push_back(t.id);

    for (int id : due_ids) {
        auto it = timers_.end();
        for (auto p = timers_.begin(); p != timers_.end(); ++p)
            if (p->id == id) { it = p; break; }
        if (it == timers_.end()) continue;

        int ref = it->ref;
        bool repeat = it->repeat;
        if (repeat) it->due = now + std::chrono::microseconds((long long)(it->interval_ms * 1000.0));

        deadline_ = std::chrono::steady_clock::now() + time_budget_;
        lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
        if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
            const char* msg = lua_tostring(L_, -1);
            log(std::string("[timer] ") + (msg ? msg : "?"));
            lua_pop(L_, 1);
        }
        if (!repeat) clear_timer(id);
    }
}

int ScriptEngine::add_raf(int ref) {
    if (!L_ || raf_.size() >= kMaxRaf) {
        if (L_) luaL_unref(L_, LUA_REGISTRYINDEX, ref);
        return 0;
    }
    int id = next_raf_id_++;
    raf_.push_back({ id, ref });
    return id;
}

void ScriptEngine::cancel_raf(int id) {
    for (size_t i = 0; i < raf_.size(); ++i) {
        if (raf_[i].first == id) {
            if (L_) luaL_unref(L_, LUA_REGISTRYINDEX, raf_[i].second);
            raf_.erase(raf_.begin() + i);
            return;
        }
    }
}

void ScriptEngine::run_raf() {
    if (!L_ || raf_.empty()) return;
    std::vector<std::pair<int, int>> due;
    due.swap(raf_);
    double ts = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start_).count();
    for (auto& [id, ref] : due) {
        deadline_ = std::chrono::steady_clock::now() + time_budget_;
        lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
        lua_pushnumber(L_, ts);
        if (lua_pcall(L_, 1, 0, 0) != LUA_OK) {
            const char* msg = lua_tostring(L_, -1);
            log(std::string("[raf] ") + (msg ? msg : "?"));
            lua_pop(L_, 1);
        }
        luaL_unref(L_, LUA_REGISTRYINDEX, ref);
    }
}

std::optional<std::chrono::steady_clock::time_point> ScriptEngine::next_wake() const {
    if (!L_) return std::nullopt;
    if (!raf_.empty()) return std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(fetch_inbox_->m);
        if (!fetch_inbox_->done.empty()) return std::chrono::steady_clock::now();
    }
    std::optional<std::chrono::steady_clock::time_point> earliest;
    for (const Timer& t : timers_)
        if (!earliest || t.due < *earliest) earliest = t.due;
    return earliest;
}
