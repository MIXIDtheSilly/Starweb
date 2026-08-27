#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <future>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>
#include <cmath>
#include <chrono>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#if defined(_WIN32)
// DWM's per-pixel-alpha compositing (GLFW_TRANSPARENT_FRAMEBUFFER) doesn't reach the
// real screen under some display paths (e.g. remote/virtual display adapters), so the
// rounded window shape is additionally enforced via a hard Win32 clip region, which
// works regardless of compositor alpha blending.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include "../common/url_parser.hpp"
#include "../common/stwp_msg.hpp"
#include "../common/net.hpp"

#include "types.hpp"
#include "globals.hpp"
#include "theme.hpp"
#include "parser.hpp"
#include "fetcher.hpp"
#include "renderer.hpp"
#include "media_player.hpp"
#include "script.hpp"
#include "devtools.hpp"
#include <filesystem>
#include <fstream>
#include <memory>

// Window chrome metrics, halved from a design drawn at 2048 wide against this
// window's 1024 logical default. Logical pixels; the chrome is a fixed frame.
namespace Trim {
    constexpr float kStripH        = 40.0f;   // tab strip
    constexpr float kToolbarH      = 40.0f;
    // The traffic lights own the left of the strip; tabs start clear of them.
    constexpr float kTabsX         = 85.0f;
    constexpr float kTabMaxW       = 175.0f;
    constexpr float kTabMinW       = 46.0f;
    constexpr float kTabH          = 29.0f;   // the active tab's outline, inset in the strip
    constexpr float kTabRounding   = 4.5f;
    constexpr float kFavicon       = 14.0f;
    constexpr float kTabPadX       = 8.0f;    // favicon's left inset within the tab
    constexpr float kTabGapX       = 8.0f;    // favicon to label
    constexpr float kCloseBox      = 16.0f;   // Lucide viewBox; the glyph is half of it
    constexpr float kCloseInset    = 14.5f;   // from the tab's right edge to the X's centre
    constexpr float kPlusSize      = 24.0f;
    constexpr float kPlusGap       = 10.5f;  // last tab's edge to the + button
    constexpr float kPlusGlyph     = 12.0f;
    constexpr float kIcon          = 16.0f;   // toolbar Lucide viewBox
    constexpr float kIconArrow     = 21.0f;   // smaller glyph in a bigger viewBox, to match rotate-cw/lock
    constexpr float kIconStroke    = 1.25f;   // the design strokes thinner than Lucide's 2/24
    constexpr float kIconStep      = 31.0f;   // centre to centre along the toolbar
    constexpr float kIconFirstX    = 22.5f;
    constexpr float kOmniboxX      = 139.5f;
    constexpr float kOmniboxH      = 28.0f;
    constexpr float kOmniboxRound  = 7.0f;
    constexpr float kOmniboxStroke = 2.0f;
    // The page is a panel floating on the chrome, not a region of it.
    constexpr float kPageInset     = 7.0f;
    constexpr float kPageTopGap    = 8.0f;
    constexpr float kPageRounding  = 8.0f;
    constexpr float kWindowRound   = 15.0f;
}

static std::unordered_map<int, std::unique_ptr<ScriptEngine>> g_script_engines;

// Drained once per frame. Page scripts run while fetch_mutex is held and
// start_async_fetch takes that same lock, so navigating straight from the
// callback deadlocks the main thread.
static std::vector<std::pair<int, std::string>> g_pending_navs;

static void run_page_scripts(Tab& tab) {
    int tid = tab.id;
    auto& eng = g_script_engines[tid];
    eng = std::make_unique<ScriptEngine>(
        [tid](const std::string& s) {
            std::cerr << "[lua " << tid << "] " << s << "\n";
            devtools::log(tid, devtools::Level::Log, s);
        },
        [tid](const std::string& s) {
            if (Tab* t = find_tab_by_id(tid)) { t->alert_text = s; t->show_alert = true; }
        });
    eng->set_tab_id(tid);
    eng->set_error_sink([tid](const std::string& s) {
        std::cerr << "[lua " << tid << "] " << s << "\n";
        devtools::log(tid, devtools::Level::Error, s);
    });
    eng->set_dom_provider([tid]() -> DomNode* {
        Tab* t = find_tab_by_id(tid);
        return t ? &t->page_dom : nullptr;
    });
    // Fetch completions land on a worker thread; without this the loop would sit in
    // glfwWaitEventsTimeout until the next heartbeat before running the callback.
    eng->set_wake([]() { glfwPostEmptyEvent(); });
    eng->set_url_provider([tid]() -> std::string {
        Tab* t = find_tab_by_id(tid);
        return t ? t->current_url : std::string();
    });
    eng->set_nav([tid](const std::string& raw) {
        Tab* t = find_tab_by_id(tid);
        if (!t) return;
        std::string url = resolve_url(t->current_url, raw);
        const bool to_moon = url.rfind("moon://", 0) == 0;
        const bool to_star = url.rfind("star://", 0) == 0;
        if (!to_moon && !to_star) {
            std::cerr << "[lua " << tid << "] blocked navigation to " << url << "\n";
            devtools::log(tid, devtools::Level::Warn, "blocked navigation to " + url);
            return;
        }
        // Stricter than the web: no script-driven downgrade. Typing the moon://
        // URL by hand still works.
        if (to_moon && t->current_url.rfind("star://", 0) == 0) {
            std::cerr << "[lua " << tid << "] blocked downgrade navigation to " << url << "\n";
            devtools::log(tid, devtools::Level::Warn, "blocked downgrade navigation to " + url);
            return;
        }
        g_pending_navs.emplace_back(tid, url);
    });
    eng->bind_inline_handlers();
    for (const PageScript& script : tab.active_page.scripts) {
        // An external script that failed to load has no source; the fetcher already
        // reported why, so skip it rather than reporting an empty chunk again.
        if (script.source.empty()) continue;
        std::string chunk = script.src.empty() ? "page" : script.src;
        std::string err;
        if (!eng->run(script.source, chunk, err)) {
            std::cerr << "[lua " << tid << "] error: " << err << "\n";
            devtools::log(tid, devtools::Level::Error, err, chunk);
        }
    }
}

ScriptEngine* script_engine_for(int tab_id) {
    auto it = g_script_engines.find(tab_id);
    return it == g_script_engines.end() ? nullptr : it->second.get();
}

void script_dispatch_click(int tab_id, uint64_t node_id) {
    auto it = g_script_engines.find(tab_id);
    if (it != g_script_engines.end() && it->second) it->second->dispatch_click(node_id);
}

bool script_has_click_handler(int tab_id, uint64_t node_id) {
    auto it = g_script_engines.find(tab_id);
    return it != g_script_engines.end() && it->second && it->second->has_click_handler(node_id);
}

void script_dispatch_input(int tab_id, uint64_t node_id) {
    auto it = g_script_engines.find(tab_id);
    if (it != g_script_engines.end() && it->second) it->second->dispatch_input(node_id);
}


static void dispatch_page_keys(GLFWwindow* window) {
    struct NamedKey { int key; const char* name; };
    static const NamedKey kNamedKeys[] = {
        {GLFW_KEY_LEFT,  "ArrowLeft"}, {GLFW_KEY_RIGHT, "ArrowRight"},
        {GLFW_KEY_UP,    "ArrowUp"},   {GLFW_KEY_DOWN,  "ArrowDown"},
        {GLFW_KEY_SPACE, " "},         {GLFW_KEY_ENTER, "Enter"},
        {GLFW_KEY_ESCAPE, "Escape"},
        {GLFW_KEY_LEFT_SHIFT, "Shift"}, {GLFW_KEY_RIGHT_SHIFT, "Shift"},
    };
    static std::unordered_map<int, bool> was_down;  // previous poll, by GLFW key code

    ScriptEngine* eng = nullptr;
    if (active_tab_idx >= 0 && active_tab_idx < (int)tabs.size()) {
        auto it = g_script_engines.find(tabs[active_tab_idx].id);
        if (it != g_script_engines.end()) eng = it->second.get();
    }
    const bool typing = ImGui::GetIO().WantTextInput;

    auto poll = [&](int key, const char* name) {
        bool down = !typing && glfwGetKey(window, key) == GLFW_PRESS;
        bool& prev = was_down[key];
        if (down == prev) return;
        prev = down;
        if (eng && eng->wants_keys()) eng->dispatch_key(down, name);
    };

    for (int k = GLFW_KEY_A; k <= GLFW_KEY_Z; k++) {
        const char name[2] = { (char)('a' + (k - GLFW_KEY_A)), '\0' };
        poll(k, name);
    }
    for (const NamedKey& nk : kNamedKeys) poll(nk.key, nk.name);
}

const std::vector<CanvasOp>* script_canvas_ops(int tab_id, uint64_t node_id) {
    auto it = g_script_engines.find(tab_id);
    if (it != g_script_engines.end() && it->second) return it->second->canvas_ops_ptr(node_id);
    return nullptr;
}

void script_set_canvas_size(int tab_id, uint64_t node_id, float w, float h) {
    auto it = g_script_engines.find(tab_id);
    if (it != g_script_engines.end() && it->second) it->second->set_canvas_size(node_id, w, h);
}

void script_set_canvas_hover(int tab_id, uint64_t node_id, float x, float y) {
    auto it = g_script_engines.find(tab_id);
    if (it != g_script_engines.end() && it->second) it->second->set_canvas_hover(node_id, x, y);
}

void script_set_canvas_font(int tab_id, uint64_t node_id, ImFont* font, float size) {
    auto it = g_script_engines.find(tab_id);
    if (it != g_script_engines.end() && it->second) it->second->set_canvas_font(node_id, font, size);
}

// Frames to keep drawing after the UI was last busy. ImGui resolves interaction
// over several frames (hover, popups, scroll targets), so going straight to sleep
// on the first quiet frame would leave the last one unpainted.
static constexpr int kSettleFrames = 3;

// How long the main loop may block waiting for input, or 0 when something on
// screen still has to animate and we need the next frame immediately. Called on
// the render thread, which is the only writer of the state it reads.
static double idle_wait_seconds(GLFWwindow* window) {
    // Upper bound on wake latency for anything not covered below.
    constexpr double kHeartbeat = 0.5;

    // A held button means a drag or resize, both of which move the window a
    // frame at a time from the render loop. Sleeping through one leaves the
    // window trailing the cursor.
    if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) return 0.0;

    for (const Tab& tab : tabs) {
        if (tab.is_fetching) return 0.0; // loading spinner
        for (const auto& [url, player] : tab.active_players)
            if (player && player->is_playing()) return 0.0;
    }

    double timeout = kHeartbeat;
    auto now = std::chrono::steady_clock::now();
    for (const auto& [id, eng] : g_script_engines) {
        if (!eng) continue;
        auto wake = eng->next_wake();
        if (!wake) continue;
        double secs = std::chrono::duration<double>(*wake - now).count();
        if (secs <= 0.0) return 0.0;
        timeout = std::min(timeout, secs);
    }
    return timeout;
}

static double idle_wait_reported(GLFWwindow* window) {
    double secs = idle_wait_seconds(window);
    devtools::note_idle_wait(secs);
    return secs;
}

#define STB_IMAGE_IMPLEMENTATION
#include "../thirdparty/stb_image.h"

#define NANOSVG_IMPLEMENTATION
#include "../thirdparty/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "../thirdparty/nanosvgrast.h"

// stb_image can't decode SVG, and extension/content-type aren't reliable, so
// sniff the bytes instead.
static bool looks_like_svg(const unsigned char* data, int size) {
    int scan = std::min(size, 512);
    int i = 0;
    if (scan >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) i = 3; // UTF-8 BOM
    while (i < scan && std::isspace(data[i])) i++;

    auto starts_with = [&](const char* s) {
        size_t len = std::strlen(s);
        return (size_t)(scan - i) >= len && std::memcmp(data + i, s, len) == 0;
    };
    if (starts_with("<svg")) return true;
    if (starts_with("<?xml")) {
        for (int j = i; j + 4 <= scan; j++) {
            if (std::memcmp(data + j, "<svg", 4) == 0) return true;
        }
    }
    return false;
}

static void upload_rgba_texture(const unsigned char* pixels, int w, int h, unsigned int* out_texture) {
    GLuint image_texture;
    glGenTextures(1, &image_texture);
    glBindTexture(GL_TEXTURE_2D, image_texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

#if defined(GL_UNPACK_ROW_LENGTH) && !defined(__EMSCRIPTEN__)
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    *out_texture = image_texture;
}

// Rasterizes at 2x for retina sharpness, capped so a bad viewBox can't
// allocate a huge texture.
static bool LoadSvgTextureFromMemory(const unsigned char* image_data, int image_size, unsigned int* out_texture, int* out_width, int* out_height) {
    std::vector<char> buf((const char*)image_data, (const char*)image_data + image_size);
    buf.push_back('\0');

    NSVGimage* image = nsvgParse(buf.data(), "px", 96.0f);
    if (!image) return false;
    if (image->width <= 0.0f || image->height <= 0.0f) {
        nsvgDelete(image);
        return false;
    }

    constexpr float kSuperSample = 2.0f;
    constexpr int kMaxDim = 2048;
    float scale = kSuperSample;
    int raster_w = std::max(1, (int)(image->width * scale));
    int raster_h = std::max(1, (int)(image->height * scale));
    if (raster_w > kMaxDim || raster_h > kMaxDim) {
        scale *= (float)kMaxDim / (float)std::max(raster_w, raster_h);
        raster_w = std::max(1, (int)(image->width * scale));
        raster_h = std::max(1, (int)(image->height * scale));
    }

    std::vector<unsigned char> pixels((size_t)raster_w * (size_t)raster_h * 4);
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) {
        nsvgDelete(image);
        return false;
    }
    nsvgRasterize(rast, image, 0.0f, 0.0f, scale, pixels.data(), raster_w, raster_h, raster_w * 4);
    nsvgDeleteRasterizer(rast);

    // Layout sees the SVG's own size, not the supersampled texture.
    *out_width = (int)image->width;
    *out_height = (int)image->height;
    nsvgDelete(image);

    upload_rgba_texture(pixels.data(), raster_w, raster_h, out_texture);
    return true;
}

bool LoadTextureFromMemory(const unsigned char* image_data, int image_size, unsigned int* out_texture, int* out_width, int* out_height) {
    if (looks_like_svg(image_data, image_size) &&
        LoadSvgTextureFromMemory(image_data, image_size, out_texture, out_width, out_height)) {
        return true;
    }

    int image_width = 0;
    int image_height = 0;
    unsigned char* data = stbi_load_from_memory(image_data, image_size, &image_width, &image_height, NULL, 4);
    if (data == NULL) return false;

    upload_rgba_texture(data, image_width, image_height, out_texture);
    stbi_image_free(data);

    *out_width = image_width;
    *out_height = image_height;

    return true;
}

int main() {
    net::Startup net_startup;
    std::filesystem::create_directories("cache");
    prune_media_cache(256ull * 1024 * 1024);
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return 1;
    }

#if defined(__APPLE__)
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    // STARWEB_SIZE=WxH opens at a given size, which is how a layout gets checked
    // at more than one window shape without dragging the corner.
    int win_w = 1024, win_h = 768;
    if (const char* size = std::getenv("STARWEB_SIZE")) {
        int w = 0, h = 0;
        if (std::sscanf(size, "%dx%d", &w, &h) == 2 && w > 320 && h > 240) {
            win_w = w;
            win_h = h;
        }
    }
    GLFWwindow* window = glfwCreateWindow(win_w, win_h, "Starmap", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

#if defined(_WIN32)
    const char* main_font_candidates[] = { "C:\\Windows\\Fonts\\arial.ttf" };
    const char* mono_font_candidates[] = { "C:\\Windows\\Fonts\\cour.ttf" };
    const char* cjk_fonts[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\msgothic.ttc",
        "C:\\Windows\\Fonts\\simsun.ttc"
    };
#elif defined(__linux__)
    const char* main_font_candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"
    };
    const char* mono_font_candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"
    };
    const char* cjk_fonts[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc"
    };
#else
    const char* main_font_candidates[] = { "/System/Library/Fonts/Supplemental/Arial.ttf" };
    const char* mono_font_candidates[] = { "/System/Library/Fonts/Supplemental/Courier New.ttf" };
    const char* cjk_fonts[] = {
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc"
    };
#endif
    const char* cjk_font_path = nullptr;
    for (const char* path : cjk_fonts) {
        if (std::filesystem::exists(path)) {
            cjk_font_path = path;
            break;
        }
    }
    const char* main_font_path = nullptr;
    for (const char* path : main_font_candidates) {
        if (std::filesystem::exists(path)) { main_font_path = path; break; }
    }
    const char* mono_font_path = nullptr;
    for (const char* path : mono_font_candidates) {
        if (std::filesystem::exists(path)) { mono_font_path = path; break; }
    }

    ImFontConfig merge_cfg;
    merge_cfg.MergeMode = true;
    merge_cfg.PixelSnapH = true;

    ImFont* font = main_font_path ? io.Fonts->AddFontFromFileTTF(main_font_path, 16.0f) : nullptr;
    if (font == nullptr) {
        font = io.Fonts->AddFontDefault();
    }

    mono_font = mono_font_path ? io.Fonts->AddFontFromFileTTF(mono_font_path, 15.0f, nullptr, io.Fonts->GetGlyphRangesJapanese()) : nullptr;
    if (mono_font == nullptr) {
        mono_font = io.Fonts->AddFontDefault();
    }
    if (cjk_font_path != nullptr) {
        io.Fonts->AddFontFromFileTTF(cjk_font_path, 15.0f, &merge_cfg, io.Fonts->GetGlyphRangesJapanese());
    }

    // Bundled faces a page can ask for by name. Added after the CJK merge above,
    // which folds itself into whichever font was registered last.
    const std::pair<const char*, const char*> bundled_fonts[] = {
        { "Inter SemiBold", "Inter-SemiBold.ttf" },
    };
    for (const auto& [family, file] : bundled_fonts) {
        std::filesystem::path path = app_dir() / "fonts" / file;
        if (std::filesystem::exists(path)) {
            register_page_font(family, io.Fonts->AddFontFromFileTTF(path.string().c_str(), 16.0f));
        }
    }

    ImGui::StyleColorsDark();

    auto& style = ImGui::GetStyle();
    style.WindowRounding = Trim::kWindowRound;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.ChildRounding = Trim::kPageRounding;

    style.Colors[ImGuiCol_WindowBg] = Theme::window_bg;
    style.Colors[ImGuiCol_ChildBg] = Theme::child_bg;
    style.Colors[ImGuiCol_PopupBg] = Theme::popup_bg;
    style.Colors[ImGuiCol_Border] = Theme::border;
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    style.Colors[ImGuiCol_FrameBg] = Theme::frame_bg;
    style.Colors[ImGuiCol_FrameBgHovered] = Theme::frame_bg_hovered;
    style.Colors[ImGuiCol_FrameBgActive] = Theme::frame_bg_active;

    style.Colors[ImGuiCol_Header] = Theme::header;
    style.Colors[ImGuiCol_HeaderHovered] = Theme::header_hovered;
    style.Colors[ImGuiCol_HeaderActive] = Theme::header_active;

    style.Colors[ImGuiCol_Button] = Theme::button;
    style.Colors[ImGuiCol_ButtonHovered] = Theme::button_hovered;
    style.Colors[ImGuiCol_ButtonActive] = Theme::button_active;

    // The scrollbar floats on the page instead of running in a channel of its
    // own: no rail behind the grab, so nothing paints a strip down the page's
    // right edge. The grab is a thin pill and the only part still drawn.
    style.ScrollbarSize = 10.0f;
    style.ScrollbarRounding = 5.0f;
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    style.Colors[ImGuiCol_ScrollbarGrab] = Theme::scrollbar_grab;
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = Theme::scrollbar_grab_hovered;
    style.Colors[ImGuiCol_ScrollbarGrabActive] = Theme::scrollbar_grab_active;

    style.Colors[ImGuiCol_CheckMark] = Theme::checkmark;
    style.Colors[ImGuiCol_SliderGrab] = Theme::slider_grab;
    style.Colors[ImGuiCol_SliderGrabActive] = Theme::slider_grab_active;

    style.Colors[ImGuiCol_InputTextCursor] = Theme::input_text_cursor;
    style.Colors[ImGuiCol_Text] = Theme::text;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    Tab initial_tab;
    initial_tab.id = next_tab_id++;
    // Development hooks, both off unless the environment sets them: STARWEB_URL
    // opens somewhere other than the local index, and STARWEB_SHOT writes the
    // framebuffer to a file and quits once the page has settled, which is how a
    // page gets looked at without a person driving the window.
    if (const char* start_url = std::getenv("STARWEB_URL")) {
        initial_tab.current_url = start_url;
        std::snprintf(initial_tab.url_input, sizeof(initial_tab.url_input), "%s", start_url);
    }
    const char* shot_path = std::getenv("STARWEB_SHOT");
    int shot_countdown = shot_path ? 240 : -1;
    tabs.push_back(initial_tab);
    active_tab_idx = 0;
    // Keys can't be injected into a headless run, so this is the only way a
    // screenshot can capture the devtools panel itself. The value names the panel
    // to open on.
    if (const char* dt = std::getenv("STARWEB_DEVTOOLS")) {
        devtools::set_open(initial_tab.id, true);
        devtools::set_panel(initial_tab.id, dt);
    }
    start_async_fetch(tabs[active_tab_idx].id, tabs[active_tab_idx].current_url);

    // ImGui rebuilds its draw data from scratch every frame, so a vsync-paced loop
    // redraws the whole window at 60fps even when the page is static. Idle frames
    // block on input instead; anything that animates asks for frames explicitly.
    int settle_frames = kSettleFrames;

    while (!glfwWindowShouldClose(window)) {
        double wait = (settle_frames > 0) ? 0.0 : idle_wait_reported(window);
        if (wait > 0.0) {
            glfwWaitEventsTimeout(wait);
        } else {
            glfwPollEvents();
        }

        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            for (size_t idx = 0; idx < tabs.size(); idx++) {
                auto& tab = tabs[idx];
                if (tab.new_page_ready) {
                    tab.is_fetching = false;
                    tab.new_page_ready = false;
                    tab.reset_scroll_next_frame = true;
                    tab.vp_slack = 0.0f;
                    tab.vp_last_h = 0.0f;
                    devtools::on_navigate(tab.id);

                    for (const auto& [url, tex] : tab.page_textures) {
                        if (tex.id != 0) {
                            glDeleteTextures(1, &tex.id);
                        }
                    }
                    tab.page_textures.clear();

                    for (auto& [url, player] : tab.active_players) {
                        delete player;
                    }
                    tab.active_players.clear();

                    if (tab.active_page.success) {
                        // Replaced only once the new one has decoded, so a page
                        // without an icon does not blank a tab mid-navigation.
                        if (!tab.active_page.favicon_bytes.empty()) {
                            TextureInfo fav;
                            if (LoadTextureFromMemory(
                                    (const unsigned char*)tab.active_page.favicon_bytes.data(),
                                    (int)tab.active_page.favicon_bytes.size(),
                                    &fav.id, &fav.width, &fav.height)) {
                                if (tab.favicon.id != 0) glDeleteTextures(1, &tab.favicon.id);
                                tab.favicon = fav;
                            }
                        } else if (tab.favicon.id != 0) {
                            glDeleteTextures(1, &tab.favicon.id);
                            tab.favicon = TextureInfo{};
                        }

                        for (const auto& [url, bytes] : tab.active_page.fetched_images) {
                            TextureInfo tex;
                            if (LoadTextureFromMemory(
                                (const unsigned char*)bytes.data(),
                                (int)bytes.size(),
                                &tex.id,
                                &tex.width,
                                &tex.height
                            )) {
                                tab.page_textures[url] = tex;
                            }
                        }
                        tab.status_text = "Success (" + std::to_string(tab.active_page.status_code) + " " + tab.active_page.status_text + ")";
                        tab.page_dom = std::move(tab.active_page.dom);
                        tab.css_classes = std::move(tab.active_page.css_classes);
                        
                        std::string parsed_title = find_title_in_dom(tab.page_dom);
                        if (!parsed_title.empty()) {
                            tab.title = trim_spaces(parsed_title);
                        } else {
                            auto opt_parsed = parse_url(tab.current_url);
                            if (opt_parsed) {
                                tab.title = opt_parsed->host + opt_parsed->path;
                            } else {
                                tab.title = "Starmap";
                            }
                        }
                        
                        run_page_scripts(tab);
                        // Dev hook, alongside STARWEB_DEVTOOLS: preselects an
                        // element so a headless shot can capture the inspector.
                        if (const char* sel = std::getenv("STARWEB_DEVTOOLS_SELECT")) {
                            devtools::select_node(tab, sel);
                        }
                        if (const char* src = std::getenv("STARWEB_DEVTOOLS_SOURCE")) {
                            devtools::select_source(tab.id, src);
                        }
                    } else {
                        tab.status_text = "Error: " + tab.active_page.error_message;
                        // Also on stderr: an interstitial says a load failed, but
                        // not which URL or when, which is exactly what you need
                        // when the same page keeps showing the same error.
                        std::cerr << "[Load failed] " << tab.current_url << " -> "
                                  << tab.active_page.error_message << "\n";
                        devtools::log(tab.id, devtools::Level::Error,
                                      "load failed: " + tab.active_page.error_message,
                                      tab.current_url);
                        std::string error_html;
                        if (tab.active_page.tls_error) {
                            auto p = parse_url(tab.current_url);
                            std::string host = p ? p->host : tab.current_url;
                            error_html =
                                "<h1 style=\"color: #e57373;\">Your connection is not private</h1>"
                                "<p>StarWeb cannot verify that this server is <b>" + host + "</b>, "
                                "so the page was not loaded and no data was sent.</p>"
                                "<p><code>" + tab.active_page.error_message + "</code></p>"
                                "<p>The certificate may be self-signed, expired, issued for a "
                                "different host, or signed by a CA StarWeb does not trust.</p>";
                            tab.title = "Privacy error";
                        } else {
                            error_html = "<h1>Error loading page</h1><p>" + tab.active_page.error_message + "</p>";
                            tab.title = "Error Loading";
                        }
                        std::string temp_css = "";
                        std::vector<PageScript> temp_scripts;
                        tab.page_dom = parse_html_to_dom(error_html, temp_css, temp_scripts);
                        g_script_engines.erase(tab.id);
                        tab.css_classes.clear();
                        tab.alert_text = "";
                    }
                    
                    if (idx == (size_t)active_tab_idx) {
                        glfwSetWindowTitle(window, ("Starmap - " + tab.title).c_str());
                    }
                }
            }
        }

        dispatch_page_keys(window);

        int visible_tab_id = (active_tab_idx >= 0 && active_tab_idx < (int)tabs.size()
                              && !glfwGetWindowAttrib(window, GLFW_ICONIFIED))
                           ? tabs[active_tab_idx].id : -1;
        for (auto& [id, eng] : g_script_engines)
            if (eng) {
                eng->set_visible(id == visible_tab_id);
                eng->poll_fetches(); eng->poll_timers(); eng->run_raf();
            }

        if (!g_pending_navs.empty()) {
            auto navs = std::move(g_pending_navs);
            g_pending_navs.clear();
            for (const auto& [tid, url] : navs) start_async_fetch(tid, url);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (active_tab_idx >= 0 && active_tab_idx < (int)tabs.size()) {
            const int tid = tabs[active_tab_idx].id;
            devtools::begin_frame(tid);
            // Dev hook: names a request by any part of its URL. Retried because
            // records only reach the store on the drain above.
            static bool net_pick_pending = std::getenv("STARWEB_DEVTOOLS_NET") != nullptr;
            if (net_pick_pending) {
                net_pick_pending =
                    !devtools::select_request(tid, std::getenv("STARWEB_DEVTOOLS_NET"));
            }
            const bool cmd = io.KeySuper || io.KeyCtrl;
            if (ImGui::IsKeyPressed(ImGuiKey_F12, false) ||
                (cmd && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_I, false))) {
                devtools::toggle(tid);
                settle_frames = kSettleFrames;
            }
        }

        {
            static int current_resize_dir = 0;
            static ImVec2 resize_start_mouse;
            static int resize_start_win_x, resize_start_win_y;
            static int resize_start_win_w, resize_start_win_h;
            
            enum {
                RESIZE_NONE = 0,
                RESIZE_LEFT = 1 << 0,
                RESIZE_RIGHT = 1 << 1,
                RESIZE_TOP = 1 << 2,
                RESIZE_BOTTOM = 1 << 3
            };
            
            int ww, wh;
            glfwGetWindowSize(window, &ww, &wh);
            double mx, my;
            glfwGetCursorPos(window, &mx, &my);
            
            const float border_size = 6.0f;
            int hover_dir = RESIZE_NONE;
            
            if (!is_window_maximized) {
                if (mx >= 0 && mx < ww && my >= 0 && my < wh) {
                    if (mx < border_size) hover_dir |= RESIZE_LEFT;
                    else if (mx >= ww - border_size) hover_dir |= RESIZE_RIGHT;
                    
                    if (my < border_size) hover_dir |= RESIZE_TOP;
                    else if (my >= wh - border_size) hover_dir |= RESIZE_BOTTOM;
                }
            }
            
            if (current_resize_dir == RESIZE_NONE) {
                if (hover_dir != RESIZE_NONE && ImGui::IsMouseClicked(0)) {
                    current_resize_dir = hover_dir;
                    resize_start_mouse = ImVec2((float)mx, (float)my);
                    glfwGetWindowPos(window, &resize_start_win_x, &resize_start_win_y);
                    glfwGetWindowSize(window, &resize_start_win_w, &resize_start_win_h);
                }
            }
            
            if (current_resize_dir != RESIZE_NONE) {
                if (ImGui::IsMouseDown(0)) {
                    int current_win_x, current_win_y;
                    glfwGetWindowPos(window, &current_win_x, &current_win_y);
                    double curr_mx, curr_my;
                    glfwGetCursorPos(window, &curr_mx, &curr_my);
                    
                    ImVec2 start_mouse_screen = ImVec2((float)resize_start_win_x + resize_start_mouse.x, (float)resize_start_win_y + resize_start_mouse.y);
                    ImVec2 curr_mouse_screen = ImVec2((float)current_win_x + (float)curr_mx, (float)current_win_y + (float)curr_my);
                    ImVec2 delta = ImVec2(curr_mouse_screen.x - start_mouse_screen.x, curr_mouse_screen.y - start_mouse_screen.y);
                    
                    int new_w = resize_start_win_w;
                    int new_h = resize_start_win_h;
                    int new_x = resize_start_win_x;
                    int new_y = resize_start_win_y;
                    
                    if (current_resize_dir & RESIZE_LEFT) {
                        new_w = resize_start_win_w - (int)delta.x;
                        new_x = resize_start_win_x + (int)delta.x;
                    }
                    if (current_resize_dir & RESIZE_RIGHT) {
                        new_w = resize_start_win_w + (int)delta.x;
                    }
                    if (current_resize_dir & RESIZE_TOP) {
                        new_h = resize_start_win_h - (int)delta.y;
                        new_y = resize_start_win_y + (int)delta.y;
                    }
                    if (current_resize_dir & RESIZE_BOTTOM) {
                        new_h = resize_start_win_h + (int)delta.y;
                    }
                    
                    const int min_w = 400;
                    const int min_h = 300;
                    
                    if (new_w < min_w) {
                        if (current_resize_dir & RESIZE_LEFT) {
                            new_x = resize_start_win_x + (resize_start_win_w - min_w);
                        }
                        new_w = min_w;
                    }
                    if (new_h < min_h) {
                        if (current_resize_dir & RESIZE_TOP) {
                            new_y = resize_start_win_y + (resize_start_win_h - min_h);
                        }
                        new_h = min_h;
                    }
                    
                    glfwSetWindowPos(window, new_x, new_y);
                    glfwSetWindowSize(window, new_w, new_h);
                } else {
                    current_resize_dir = RESIZE_NONE;
                }
            }
            
            int active_dir = (current_resize_dir != RESIZE_NONE) ? current_resize_dir : hover_dir;
            if (active_dir != RESIZE_NONE) {
                if ((active_dir & RESIZE_LEFT) && (active_dir & RESIZE_TOP)) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                } else if ((active_dir & RESIZE_RIGHT) && (active_dir & RESIZE_BOTTOM)) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                } else if ((active_dir & RESIZE_RIGHT) && (active_dir & RESIZE_TOP)) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
                } else if ((active_dir & RESIZE_LEFT) && (active_dir & RESIZE_BOTTOM)) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
                } else if (active_dir & (RESIZE_LEFT | RESIZE_RIGHT)) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                } else if (active_dir & (RESIZE_TOP | RESIZE_BOTTOM)) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                }
            }
        }

        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);

#if defined(_WIN32)
        {
            static int last_region_w = -1, last_region_h = -1;
            if (display_w != last_region_w || display_h != last_region_h) {
                HWND hwnd = glfwGetWin32Window(window);
                HRGN region = CreateRoundRectRgn(0, 0, display_w, display_h, 16, 16);
                SetWindowRgn(hwnd, region, TRUE); // ownership of region transfers to the window
                last_region_w = display_w;
                last_region_h = display_h;
            }
        }
#endif

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y));
        // No padding or border on the shell; the page panel insets itself, and a
        // border would trace the rounded sheet's corner. Popped right after Begin.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("StarmapWorkspace", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar(2);

        float tab_height = Trim::kStripH;
        float max_tab_width = Trim::kTabMaxW;
        float min_tab_width = Trim::kTabMinW;

        float window_avail_width = ImGui::GetContentRegionAvail().x;
        // What is left for tabs once the traffic lights and the + button have
        // taken theirs.
        float avail_w = window_avail_width - Trim::kTabsX - Trim::kPlusSize - 24.0f;
        float tab_width = avail_w / tabs.size();
        if (tab_width > max_tab_width) tab_width = max_tab_width;
        if (tab_width < min_tab_width) tab_width = min_tab_width;

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
        
        ImVec2 bar_min = cursor_pos;
        ImVec2 bar_max = ImVec2(bar_min.x + window_avail_width, bar_min.y + tab_height);
        // The strip is the top of the window, so it carries the window's own
        // radius; filled square it painted a sharp corner over the rounded sheet
        // underneath and squared off the whole top edge.
        draw_list->AddRectFilled(bar_min, bar_max, Theme::bar_bg,
                                 Trim::kWindowRound, ImDrawFlags_RoundCornersTop);

        ImVec2 tl_pos = cursor_pos;
        ImVec2 mouse_pos = ImGui::GetIO().MousePos;
        bool mouse_clicked = ImGui::IsMouseClicked(0);
        
        // Centres at 22.5 / 42.5 / 62.5, vertically centred in the strip; the hit
        // box is the 16px square around each.
        const float tl_cy = tl_pos.y + tab_height * 0.5f;
        auto light_hovered = [&](float cx) {
            return mouse_pos.x >= tl_pos.x + cx - 8.0f && mouse_pos.x < tl_pos.x + cx + 8.0f &&
                   mouse_pos.y >= tl_cy - 8.0f && mouse_pos.y < tl_cy + 8.0f;
        };
        bool red_hovered    = light_hovered(22.5f);
        bool yellow_hovered = light_hovered(42.5f);
        bool green_hovered  = light_hovered(62.5f);

        if (mouse_clicked) {
            if (red_hovered) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            } else if (yellow_hovered) {
                glfwIconifyWindow(window);
            } else if (green_hovered) {
                if (is_window_maximized) {
                    glfwSetWindowMonitor(window, nullptr, restored_x, restored_y, restored_w, restored_h, 0);
                    is_window_maximized = false;
                } else {
                    glfwGetWindowPos(window, &restored_x, &restored_y);
                    glfwGetWindowSize(window, &restored_w, &restored_h);
                    
                    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
                    int count;
                    GLFWmonitor** monitors = glfwGetMonitors(&count);
                    if (count > 0) {
                        int win_x, win_y;
                        glfwGetWindowPos(window, &win_x, &win_y);
                        for (int m = 0; m < count; ++m) {
                            int mx, my;
                            glfwGetMonitorPos(monitors[m], &mx, &my);
                            const GLFWvidmode* mode = glfwGetVideoMode(monitors[m]);
                            if (mode) {
                                if (win_x >= mx && win_x < mx + mode->width && win_y >= my && win_y < my + mode->height) {
                                    monitor = monitors[m];
                                    break;
                                }
                            }
                        }
                    }
                    
                    int monitor_x, monitor_y, monitor_w, monitor_h;
                    glfwGetMonitorWorkarea(monitor, &monitor_x, &monitor_y, &monitor_w, &monitor_h);
                    glfwSetWindowMonitor(window, nullptr, monitor_x, monitor_y, monitor_w, monitor_h, 0);
                    is_window_maximized = true;
                }
            }
        }

        draw_list->AddCircleFilled(ImVec2(tl_pos.x + 22.5f, tl_cy), 6.0f, red_hovered ? IM_COL32(255, 70, 70, 255) : IM_COL32(255, 95, 87, 255));
        draw_list->AddCircleFilled(ImVec2(tl_pos.x + 42.5f, tl_cy), 6.0f, yellow_hovered ? IM_COL32(240, 170, 30, 255) : IM_COL32(255, 189, 46, 255));
        draw_list->AddCircleFilled(ImVec2(tl_pos.x + 62.5f, tl_cy), 6.0f, green_hovered ? IM_COL32(30, 180, 50, 255) : IM_COL32(40, 201, 64, 255));

        ImVec2 clip_min = ImVec2(cursor_pos.x + Trim::kTabsX, cursor_pos.y);
        ImVec2 clip_max = ImVec2(cursor_pos.x + window_avail_width - Trim::kPlusSize - 12.0f,
                                 cursor_pos.y + tab_height);
        draw_list->PushClipRect(clip_min, clip_max, true);

        int tab_to_close = -1;
        int tab_to_select = -1;

        for (size_t i = 0; i < tabs.size(); ++i) {
            auto& tab = tabs[i];
            bool is_active = ((int)i == active_tab_idx);
            
            ImVec2 tab_min = ImVec2(cursor_pos.x + i * tab_width + Trim::kTabsX, cursor_pos.y);
            ImVec2 tab_max = ImVec2(tab_min.x + tab_width, tab_min.y + tab_height);
            bool tab_hovered = (mouse_pos.x >= tab_min.x && mouse_pos.x < tab_max.x &&
                                mouse_pos.y >= tab_min.y && mouse_pos.y < tab_max.y);

            // Only the current tab offers a close button; the rest are a click
            // away from being current, and a strip of them reads as clutter.
            bool show_close = is_active && tab_width >= 60.0f;
            bool close_hovered = show_close && tab_hovered &&
                                 (mouse_pos.x >= tab_max.x - Trim::kCloseInset - 9.0f);
            bool click_hovered = tab_hovered && !close_hovered;

            if (mouse_clicked) {
                if (close_hovered) {
                    tab_to_close = (int)i;
                } else if (click_hovered) {
                    tab_to_select = (int)i;
                }
            }

            // The box is inset in the strip rather than filling it, and it is an
            // outline, not a fill: an inactive tab has no box at all, so hover is
            // the only fill the strip ever shows.
            const float pill_pad = (tab_height - Trim::kTabH) * 0.5f;
            ImVec2 pill_min = ImVec2(tab_min.x + 0.5f, tab_min.y + pill_pad);
            ImVec2 pill_max = ImVec2(tab_max.x - 0.5f, tab_min.y + pill_pad + Trim::kTabH);

            if (is_active) {
                draw_list->AddRectFilled(pill_min, pill_max, Theme::bar_bg, Trim::kTabRounding);
                draw_list->AddRect(pill_min, pill_max, Theme::outline_mid, Trim::kTabRounding, 0, 1.0f);
            } else if (tab_hovered) {
                draw_list->AddRectFilled(pill_min, pill_max, Theme::tab_hover_bg, Trim::kTabRounding);
            }

            float text_center_y = std::round(tab_min.y + tab_height * 0.5f);

            // Favicon, then label. Without one the label takes the space back, so
            // a tab that has not resolved an icon yet does not sit indented.
            float content_x = tab_min.x + Trim::kTabPadX;
            const TextureInfo& fav = tab.favicon;
            if (fav.id != 0 && tab_width >= 70.0f) {
                ImVec2 fav_min = ImVec2(std::round(content_x),
                                        std::round(text_center_y - Trim::kFavicon * 0.5f));
                ImVec2 fav_max = ImVec2(fav_min.x + Trim::kFavicon, fav_min.y + Trim::kFavicon);
                draw_list->AddImage((ImTextureID)(intptr_t)fav.id, fav_min, fav_max);
                content_x = fav_max.x + Trim::kTabGapX;
            }

            float label_right = tab_max.x - (show_close ? Trim::kCloseInset + 10.0f : Trim::kTabPadX);
            if (tab_width >= 70.0f && label_right > content_x + 8.0f) {
                float text_y = std::round(text_center_y - ImGui::GetFontSize() * 0.5f);
                ImVec2 text_min = ImVec2(std::round(content_x), text_y);
                ImVec2 text_max = ImVec2(label_right, text_y + ImGui::GetFontSize());
                ImU32 label_col = is_active ? Theme::tab_text_on : Theme::tab_text_off;
                draw_list->PushClipRect(text_min, text_max, true);
                draw_list->AddText(text_min, label_col, tab.title.c_str());
                draw_list->PopClipRect();

                // Fade the last few pixels rather than chopping a glyph in half.
                // The strip is flat black behind every tab state except hover, and
                // an outlined active tab is black inside too, so one colour serves.
                ImU32 fade_bg = (is_active || !tab_hovered) ? Theme::bar_bg : Theme::tab_hover_bg;
                ImU32 fade_clear = fade_bg & 0x00FFFFFF;
                draw_list->AddRectFilledMultiColor(
                    ImVec2(text_max.x - 18.0f, text_min.y), text_max,
                    fade_clear, fade_bg, fade_bg, fade_clear);
            }

            if (show_close) {
                ImVec2 x_center = ImVec2(std::round(tab_max.x - Trim::kCloseInset), text_center_y);
                ImU32 x_color = close_hovered ? IM_COL32(240, 240, 240, 255)
                                              : (is_active ? Theme::tab_text_on : Theme::tab_text_off);
                if (close_hovered) {
                    draw_list->AddRectFilled(ImVec2(x_center.x - 9.0f, x_center.y - 9.0f),
                                             ImVec2(x_center.x + 9.0f, x_center.y + 9.0f),
                                             Theme::tab_close_hover_bg, 4.0f);
                }
                DrawXIcon(x_center, x_color, Trim::kCloseBox, Trim::kIconStroke);
            }
        }

        draw_list->PopClipRect();

        float max_plus_x = cursor_pos.x + window_avail_width - Trim::kPlusSize - 12.0f;
        float plus_x = cursor_pos.x + tabs.size() * tab_width + Trim::kTabsX + Trim::kPlusGap;
        if (plus_x > max_plus_x) plus_x = max_plus_x;

        ImVec2 plus_min = ImVec2(std::round(plus_x),
                                 std::round(cursor_pos.y + (tab_height - Trim::kPlusSize) * 0.5f));
        ImVec2 plus_max = ImVec2(plus_min.x + Trim::kPlusSize, plus_min.y + Trim::kPlusSize);

        bool plus_hovered = (mouse_pos.x >= plus_min.x && mouse_pos.x < plus_max.x &&
                             mouse_pos.y >= plus_min.y && mouse_pos.y < plus_max.y);
        bool plus_active = plus_hovered && ImGui::IsMouseDown(0);

        if (plus_hovered && mouse_clicked) {
            Tab new_tab;
            new_tab.id = next_tab_id++;
            tabs.push_back(new_tab);
            active_tab_idx = (int)tabs.size() - 1;
            start_async_fetch(tabs[active_tab_idx].id, tabs[active_tab_idx].current_url);
            glfwSetWindowTitle(window, ("Starmap - " + tabs[active_tab_idx].title).c_str());
        }
        
        // Outlined like the active tab but one step dimmer on the border ramp: it
        // is an affordance sitting at rest, not the current selection.
        if (plus_active || plus_hovered) {
            draw_list->AddRectFilled(plus_min, plus_max,
                                     plus_active ? Theme::plus_bg_active : Theme::plus_bg_hover,
                                     Trim::kTabRounding);
        }
        draw_list->AddRect(plus_min, plus_max,
                           plus_hovered ? Theme::outline_mid : Theme::outline_dim,
                           Trim::kTabRounding, 0, 1.0f);

        ImVec2 plus_center = ImVec2((plus_min.x + plus_max.x) * 0.5f, (plus_min.y + plus_max.y) * 0.5f);
        ImU32 plus_color = plus_hovered ? Theme::plus_color_hover : Theme::plus_color_normal;
        DrawPlusIcon(plus_center, plus_color, Trim::kPlusGlyph * 24.0f / 14.0f, Trim::kIconStroke);

        if (tab_to_select != -1) {
            active_tab_idx = tab_to_select;
            glfwSetWindowTitle(window, ("Starmap - " + tabs[active_tab_idx].title).c_str());
        }
        if (tab_to_close != -1) {
            if (net::is_valid(tabs[tab_to_close].active_socket_fd)) {
                net::close(tabs[tab_to_close].active_socket_fd);
                tabs[tab_to_close].active_socket_fd = net::kInvalidSocket;
            }
            
            // Delete its textures first!
            for (const auto& [url, tex] : tabs[tab_to_close].page_textures) {
                if (tex.id != 0) {
                    glDeleteTextures(1, &tex.id);
                }
            }
            tabs[tab_to_close].page_textures.clear();
            if (tabs[tab_to_close].favicon.id != 0) {
                glDeleteTextures(1, &tabs[tab_to_close].favicon.id);
                tabs[tab_to_close].favicon = TextureInfo{};
            }

            for (auto& [url, player] : tabs[tab_to_close].active_players) {
                delete player;
            }
            tabs[tab_to_close].active_players.clear();
            g_script_engines.erase(tabs[tab_to_close].id);
            devtools::on_tab_closed(tabs[tab_to_close].id);

            tabs.erase(tabs.begin() + tab_to_close);
            if (tabs.empty()) {
                Tab new_tab;
                new_tab.id = next_tab_id++;
                tabs.push_back(new_tab);
                active_tab_idx = 0;
                start_async_fetch(tabs[active_tab_idx].id, tabs[active_tab_idx].current_url);
            } else {
                if (active_tab_idx >= (int)tabs.size()) {
                    active_tab_idx = (int)tabs.size() - 1;
                }
            }
            glfwSetWindowTitle(window, ("Starmap - " + tabs[active_tab_idx].title).c_str());
        }

        // No rule under the strip: the design separates the chrome from the page
        // with the panel's inset and corner radius instead of a line, and the
        // active tab is a self-contained outline that owes nothing to the edge.

        static bool is_dragging = false;
        static double drag_start_x = 0;
        static double drag_start_y = 0;
        if (ImGui::IsMouseClicked(0)) {
            ImVec2 m_pos = ImGui::GetIO().MousePos;
            ImVec2 w_pos = ImGui::GetWindowPos();
            if (m_pos.y >= w_pos.y + 6.0f && m_pos.y <= w_pos.y + tab_height && m_pos.x >= w_pos.x && m_pos.x < w_pos.x + window_avail_width) {
                bool over_interactive = false;
                if (m_pos.x < w_pos.x + Trim::kTabsX) over_interactive = true;

                for (size_t i = 0; i < tabs.size(); ++i) {
                    float t_min_x = w_pos.x + Trim::kTabsX + i * tab_width;
                    float t_max_x = t_min_x + tab_width;
                    if (m_pos.x >= t_min_x && m_pos.x <= t_max_x) {
                        over_interactive = true;
                        break;
                    }
                }

                float plus_start_x = w_pos.x + Trim::kTabsX + tabs.size() * tab_width + Trim::kPlusGap;
                if (m_pos.x >= plus_start_x && m_pos.x <= plus_start_x + Trim::kPlusSize + 8.0f) {
                    over_interactive = true;
                }
                
                if (!over_interactive) {
                    if (is_window_maximized) {
                        double mouse_x_on_screen, mouse_y_on_screen;
                        glfwGetCursorPos(window, &mouse_x_on_screen, &mouse_y_on_screen);
                        int win_x, win_y;
                        glfwGetWindowPos(window, &win_x, &win_y);
                        
                        double absolute_mouse_x = win_x + mouse_x_on_screen;
                        float click_pct = (float)mouse_x_on_screen / ImGui::GetWindowWidth();
                        
                        glfwSetWindowMonitor(window, nullptr, (int)(absolute_mouse_x - restored_w * click_pct), win_y + 10, restored_w, restored_h, 0);
                        is_window_maximized = false;
                    }
                    is_dragging = true;
                    glfwGetCursorPos(window, &drag_start_x, &drag_start_y);
                }
            }
        }
        
        if (is_dragging) {
            if (ImGui::IsMouseDown(0)) {
                double curr_x, curr_y;
                glfwGetCursorPos(window, &curr_x, &curr_y);
                int win_x, win_y;
                glfwGetWindowPos(window, &win_x, &win_y);
                glfwSetWindowPos(window, win_x + (int)(curr_x - drag_start_x), win_y + (int)(curr_y - drag_start_y));
            } else {
                is_dragging = false;
            }
        }

        Tab& active_tab = tabs[active_tab_idx];

        float toolbar_height = Trim::kToolbarH;

        ImVec2 toolbar_min = ImVec2(cursor_pos.x, cursor_pos.y + tab_height);
        ImVec2 toolbar_max = ImVec2(toolbar_min.x + window_avail_width, toolbar_min.y + toolbar_height);
        draw_list->AddRectFilled(toolbar_min, toolbar_max, Theme::toolbar_bg);

        // The toolbar's controls sit at fixed centres from the design rather than
        // flowing, so the omnibox always starts in the same place whatever the
        // font metrics are. Each icon gets a hit box larger than its glyph.
        const float tb_cy = toolbar_min.y + toolbar_height * 0.5f;
        const float hit = 26.0f;
        auto icon_slot = [&](int n) {
            return ImVec2(toolbar_min.x + Trim::kIconFirstX + Trim::kIconStep * n, tb_cy);
        };
        auto icon_button = [&](const char* id, int n, bool disabled) {
            ImVec2 c = icon_slot(n);
            ImGui::SetCursorScreenPos(ImVec2(c.x - hit * 0.5f, c.y - hit * 0.5f));
            if (disabled) {
                ImGui::Dummy(ImVec2(hit, hit));
                return false;
            }
            bool clicked = ImGui::InvisibleButton(id, ImVec2(hit, hit));
            if (ImGui::IsItemHovered()) {
                draw_list->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                         ImGui::IsItemActive() ? Theme::plus_bg_active
                                                               : Theme::plus_bg_hover,
                                         Trim::kTabRounding);
            }
            return clicked;
        };

        bool back_disabled = (active_tab.history_index <= 0);
        if (icon_button("##back", 0, back_disabled)) {
            active_tab.history_index--;
            start_async_fetch(active_tab.id, active_tab.navigation_history[active_tab.history_index], true);
        }
        DrawBackArrowIcon(icon_slot(0), back_disabled ? Theme::icon_disabled : Theme::icon_normal,
                          Trim::kIconArrow, Trim::kIconStroke);

        bool forward_disabled = (active_tab.history_index >= (int)active_tab.navigation_history.size() - 1 || active_tab.navigation_history.empty());
        if (icon_button("##forward", 1, forward_disabled)) {
            active_tab.history_index++;
            start_async_fetch(active_tab.id, active_tab.navigation_history[active_tab.history_index], true);
        }
        DrawForwardArrowIcon(icon_slot(1), forward_disabled ? Theme::icon_disabled : Theme::icon_normal,
                             Trim::kIconArrow, Trim::kIconStroke);

        if (active_tab.is_fetching) {
            icon_button("##reload", 2, true);
            DrawSpinner(icon_slot(2), 7.0f, Trim::kIconStroke + 0.5f, Theme::spinner);
        } else {
            if (icon_button("##reload", 2, false)) {
                start_async_fetch(active_tab.id, active_tab.current_url);
            }
            DrawReloadIcon(icon_slot(2), Theme::icon_normal, Trim::kIcon, Trim::kIconStroke);
        }

        const FetchResult& page = active_tab.active_page;
        const bool secure = page.is_secure && page.tls.verified;
        if (icon_button("##lock", 3, false)) {
            ImGui::OpenPopup("cert_info");
        }
        bool lock_hovered = ImGui::IsItemHovered();
        DrawLockIcon(icon_slot(3), secure ? Theme::lock_secure : Theme::lock_insecure, secure,
                     Trim::kIcon, Trim::kIconStroke);
        if (lock_hovered) {
            ImGui::SetTooltip(secure ? "Connection is secure (TLS 1.3)"
                                     : "Not secure - sent in plaintext");
        }

        if (ImGui::BeginPopup("cert_info")) {
            if (secure) {
                ImGui::TextColored(ImVec4(0.47f, 0.86f, 0.55f, 1.0f), "Connection is secure");
                ImGui::Separator();
                ImGui::Text("%s, %s", page.tls.version.c_str(), page.tls.cipher.c_str());
                if (!page.tls.alpn.empty()) ImGui::Text("Protocol: %s", page.tls.alpn.c_str());
                ImGui::Spacing();
                ImGui::TextDisabled("Certificate");
                ImGui::Text("Subject: %s", page.tls.peer_subject.c_str());
                ImGui::Text("Issuer:  %s", page.tls.peer_issuer.c_str());
                ImGui::Text("Expires: %s", page.tls.not_after.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.45f, 1.0f), "Not secure");
                ImGui::Separator();
                // An auto-sizing popup has no width to wrap against, so without an
                // explicit position the text collapses to one word a line.
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 300.0f);
                ImGui::TextWrapped("This page was loaded over moon://, which sends everything "
                                   "in plaintext. Use star:// for an encrypted connection.");
                ImGui::PopTextWrapPos();
            }
            ImGui::EndPopup();
        }

        // The omnibox is a hole in the chrome, not a raised field: its fill is the
        // toolbar's own black and the outline alone gives it an edge, climbing the
        // border ramp to full brightness while it has focus.
        const float omni_x = toolbar_min.x + Trim::kOmniboxX;
        const float omni_w = window_avail_width - Trim::kOmniboxX - 8.0f;
        const float omni_y = std::round(tb_cy - Trim::kOmniboxH * 0.5f);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Trim::kOmniboxRound);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        // Vertically centre the text in a field that is taller than one line.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(12.0f, (Trim::kOmniboxH - ImGui::GetFontSize()) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Theme::omnibox_bg);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Theme::omnibox_bg);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Theme::omnibox_bg);

        ImGui::SetCursorScreenPos(ImVec2(omni_x, omni_y));
        ImGui::PushItemWidth(omni_w);
        if (ImGui::InputText("##url", active_tab.url_input, IM_ARRAYSIZE(active_tab.url_input), ImGuiInputTextFlags_EnterReturnsTrue)) {
            start_async_fetch(active_tab.id, active_tab.url_input);
        }
        const bool omni_focused = ImGui::IsItemActive();
        const bool omni_hovered = ImGui::IsItemHovered();
        ImGui::PopItemWidth();

        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(3);

        // Drawn after the field so the outline is not painted over by its fill.
        draw_list->AddRect(ImVec2(omni_x, omni_y), ImVec2(omni_x + omni_w, omni_y + Trim::kOmniboxH),
                           omni_focused ? Theme::outline_bright
                                        : (omni_hovered ? Theme::outline_mid : Theme::outline_dim),
                           Trim::kOmniboxRound, 0,
                           omni_focused ? Trim::kOmniboxStroke : 1.0f);

        // The page is a panel floating on the chrome: inset on three sides, with a
        // gap under the toolbar, and rounded on all four corners. Nothing rules it
        // off from the chrome: the inset is the separation.
        ImGui::SetCursorScreenPos(ImVec2(toolbar_min.x + Trim::kPageInset,
                                         toolbar_max.y + Trim::kPageTopGap));

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_InputTextCursor, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
        // No padding of its own: a full-bleed element needs to reach the edge,
        // and the page insets itself via body margin. Popped right after
        // Begin reads it.
        // The devtools dock eats into the page from the right. Everything the page
        // measures itself against comes from GetContentRegionAvail inside this
        // child, so vw/vh and canvas auto-fit follow the narrowed viewport.
        const ImVec2 shell_avail = ImVec2(window_avail_width - Trim::kPageInset * 2.0f,
                                          ImGui::GetContentRegionAvail().y - Trim::kPageInset);
        // The gap between the two panels is the page's own inset.
        const float dt_splitter_w = Trim::kPageInset;
        const float dt_w = devtools::dock_width(active_tab.id, shell_avail.x);
        const bool dt_open = dt_w > 0.0f;
        // Sized explicitly rather than left at 0: "fill the parent" would run to
        // the window edge and eat the panel's right inset, since the shell has no
        // padding of its own to stop at.
        const float page_w = dt_open ? shell_avail.x - dt_w - dt_splitter_w : shell_avail.x;

        // Painted on the shell, not inside the child: a child clips to its inner
        // rect (short of the scrollbar), which left a bare strip down the edge.
        // The child's own background goes transparent so this shows through.
        {
            ImVec2 panel_min = ImGui::GetCursorScreenPos();
            ImVec2 panel_max = ImVec2(panel_min.x + page_w, panel_min.y + shell_avail.y);
            const ImDrawFlags vp_corners = ImDrawFlags_RoundCornersAll;
            ImDrawList* shell_draw_list = ImGui::GetWindowDrawList();
            auto body_it = active_tab.css_classes.find("body");
            const CssStyle* body_style = body_it != active_tab.css_classes.end()
                                       ? &body_it->second : nullptr;
            if (body_style && body_style->has_gradient) {
                ImU32 col_start = ImGui::ColorConvertFloat4ToU32(body_style->gradient_start);
                ImU32 col_end = ImGui::ColorConvertFloat4ToU32(body_style->gradient_end);
                shell_draw_list->AddRectFilledMultiColor(panel_min, panel_max, col_start, col_start, col_end, col_end);
            } else {
                ImU32 fill = (body_style && body_style->has_bg)
                           ? ImGui::ColorConvertFloat4ToU32(body_style->bg_color)
                           : Theme::viewport_bg;
                shell_draw_list->AddRectFilled(panel_min, panel_max, fill,
                                               Trim::kPageRounding, vp_corners);
            }
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        // Explicit height: 0 would fill to the parent's content edge and eat the
        // panel's bottom inset.
        ImGui::BeginChild("RenderViewport", ImVec2(page_w, shell_avail.y), false, 0);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        if (active_tab.reset_scroll_next_frame) {
            ImGui::SetScrollY(0.0f);
            active_tab.reset_scroll_next_frame = false;
        }
        
        // The usable content box, not the window box, less the slack that the
        // wrapper elements around a vh-sized box turned out to cost last frame.
        ImVec2 vp_avail = ImGui::GetContentRegionAvail();
        page_viewport_w = vp_avail.x;
        page_viewport_h = vp_avail.y - active_tab.vp_slack;
        if (page_viewport_h < 1.0f) page_viewport_h = 1.0f;
        page_viewport_w_full = vp_avail.x;
        page_viewport_h_full = vp_avail.y > 1.0f ? vp_avail.y : 1.0f;

        // Vector art is drawn in logical pixels and scaled up to the framebuffer,
        // which stretches the anti-aliasing fringe with it: a hairline stroke on a
        // 2x display gets two device pixels of gradient on each side, which is
        // what makes small icons look soft. Shrink the fringe by the same factor
        // so it stays one device pixel whatever the display does.
        const float fb_scale = ImGui::GetIO().DisplayFramebufferScale.y;
        if (fb_scale > 1.0f) ImGui::GetWindowDrawList()->_FringeScale = 1.0f / fb_scale;

        // Circles and rounded corners pick their segment count from an error
        // budget in logical pixels, which the same scaling stretches into a
        // visible flat side. Tessellate for the device instead; ImGui copies this
        // to the draw data on the next NewFrame.
        if (fb_scale > 1.0f) {
            ImGui::GetStyle().CircleTessellationMaxError = 0.30f / fb_scale;
        }

        page_document_origin = ImGui::GetCursorScreenPos();
        page_viewport_origin = ImVec2(page_document_origin.x + ImGui::GetScrollX(),
                                      page_document_origin.y + ImGui::GetScrollY());

        CssStyle default_style;
        default_style.color = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);
        default_style.has_color = true;
        bool default_inline_flow = false;
        // Vertical gaps are the page's to set via margins; ImGui's own
        // ItemSpacing would stack 4px per nesting level on top, which is what
        // kept a 100vh box from reaching the bottom. X spacing stays;
        // SameLine passes its own gap explicitly.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        render_node(active_tab.page_dom, default_style, default_inline_flow, active_tab);
        ImGui::PopStyleVar();

        float vp_h = ImGui::GetWindowHeight();
        float vp_w = ImGui::GetWindowWidth();
        // Width matters as well as height: opening the dock reflows the page, so
        // what the wrappers around a vh box cost has to be measured again.
        if (vp_h != active_tab.vp_last_h || vp_w != active_tab.vp_last_w) {
            active_tab.vp_slack = 0.0f;
            active_tab.vp_last_h = vp_h;
            active_tab.vp_last_w = vp_w;
        } else if (active_tab.vp_fit_used) {
            // What a 100vh box costs beyond its own height: wrappers around it,
            // plus whatever trails after. Measured directly rather than
            // accumulated from GetScrollMaxY, which only ever grew and settled
            // short of the true cost.
            float overhead = ImGui::GetCursorPosY() - page_viewport_h;
            if (overhead < 0.0f) overhead = 0.0f;
            // A page whose other content genuinely overflows would otherwise
            // starve the vh box down to nothing.
            float cap = vp_h * 0.5f;
            if (overhead > cap) overhead = cap;
            if (std::abs(overhead - active_tab.vp_slack) > 0.5f) {
                active_tab.vp_slack = overhead;
                settle_frames = kSettleFrames;
            }
        }
        active_tab.vp_fit_used = false;

        ImGui::EndChild();
        ImGui::PopStyleColor(2);

        if (dt_open) {
            ImVec2 vp_rect_min = ImGui::GetItemRectMin();
            ImVec2 vp_rect_max = ImGui::GetItemRectMax();
            devtools::draw_overlay(active_tab, vp_rect_min, vp_rect_max);

            ImGui::SameLine(0.0f, 0.0f);
            ImGui::InvisibleButton("##dt_splitter", ImVec2(dt_splitter_w, shell_avail.y));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            if (ImGui::IsItemActive()) devtools::drag_dock(-io.MouseDelta.x);
            // Nothing at rest; a short grab handle under the cursor.
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                ImVec2 sp_min = ImGui::GetItemRectMin();
                float cx = std::round(sp_min.x + dt_splitter_w * 0.5f);
                float cy = sp_min.y + shell_avail.y * 0.5f;
                ImGui::GetWindowDrawList()->AddLine(
                    ImVec2(cx, cy - 16.0f), ImVec2(cx, cy + 16.0f),
                    ImGui::IsItemActive() ? Theme::outline_bright : Theme::outline_mid, 2.0f);
            }

            ImGui::SameLine(0.0f, 0.0f);
            // The dock never scrolls as a whole; each panel scrolls its own panes.
            // Sized explicitly for the same reason the page is: it has to stop at
            // the panel's right inset, not at the window edge.
            ImGui::BeginChild("DevTools", ImVec2(dt_w, shell_avail.y), false, ImGuiWindowFlags_NoScrollbar);
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImGui::GetWindowPos(),
                ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                       ImGui::GetWindowPos().y + ImGui::GetWindowHeight()),
                Theme::dt_bg, Trim::kPageRounding, ImDrawFlags_RoundCornersAll);
            devtools::draw(active_tab);
            ImGui::EndChild();
        }

        if (active_tab.show_alert) {
            ImGui::OpenPopup("Alert");
        }
        if (ImGui::BeginPopupModal("Alert", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s", active_tab.alert_text.c_str());
            ImGui::Separator();
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                active_tab.show_alert = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::End();

        // An auto-resizing modal needs a couple of frames to lay itself out, and it
        // can be opened by a script rather than by input, so it asks for frames too.
        bool ui_busy = io.WantTextInput || ImGui::IsAnyItemActive() || ImGui::IsAnyMouseDown() ||
                       io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f ||
                       io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f ||
                       active_tab.show_alert || devtools::wants_frames(active_tab.id);
        if (ui_busy) settle_frames = kSettleFrames;
        else if (settle_frames > 0) settle_frames--;

        ImGui::Render();
        glViewport(0, 0, display_w, display_h);
        // Transparent black: the compositor is premultiplied, so any colour left
        // here at alpha 0 still tints the wedges outside the window's rounded corners.
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (shot_countdown > 0) {
            settle_frames = kSettleFrames;  // keep drawing rather than idling
            shot_countdown--;
        } else if (shot_countdown == 0) {
            std::vector<unsigned char> px((size_t)display_w * display_h * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, display_w, display_h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
            if (FILE* f = std::fopen(shot_path, "wb")) {
                std::fprintf(f, "P6\n%d %d\n255\n", display_w, display_h);
                // GL reads bottom-up; PPM is top-down.
                size_t stride = (size_t)display_w * 3;
                for (int y = display_h - 1; y >= 0; --y) {
                    std::fwrite(px.data() + (size_t)y * stride, 1, stride, f);
                }
                std::fclose(f);
            }
            break;
        }

        glfwSwapBuffers(window);
    }

    for (auto& tab : tabs) {
        for (const auto& [url, tex] : tab.page_textures) {
            if (tex.id != 0) {
                glDeleteTextures(1, &tex.id);
            }
        }
        tab.page_textures.clear();
        if (tab.favicon.id != 0) {
            glDeleteTextures(1, &tab.favicon.id);
            tab.favicon = TextureInfo{};
        }
        for (auto& [url, player] : tab.active_players) {
            delete player;
        }
        tab.active_players.clear();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}