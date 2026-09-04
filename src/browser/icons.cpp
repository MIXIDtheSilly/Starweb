// The chrome's icons, drawn from their real Lucide SVG source rather than from
// hand-traced ImGui paths. Each one is rasterized once by nanosvg at the exact
// device-pixel size it will be drawn at, uploaded as an alpha-carrying texture,
// and tinted at draw time, so the geometry is whatever the SVG says and only
// the colour is ours.
#include "renderer.hpp"

#include "imgui.h"

#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// The implementations live in browser.cpp; these are the declarations only.
#include "../thirdparty/nanosvg.h"
#include "../thirdparty/nanosvgrast.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace {

// Lucide, verbatim. Their viewBox is 24x24, which is what `size` measures.
const char* const kArrowLeft =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m12 19-7-7 7-7"/><path d="M19 12H5"/></svg>)";

const char* const kArrowRight =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14"/><path d="m12 5 7 7-7 7"/></svg>)";

const char* const kRotateCw =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 12a9 9 0 1 1-9-9c2.52 0 4.93 1 6.74 2.74L21 8"/><path d="M21 3v5h-5"/></svg>)";

const char* const kX =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M18 6 6 18"/><path d="m6 6 12 12"/></svg>)";

const char* const kLock =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect width="18" height="11" x="3" y="11" rx="2" ry="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/></svg>)";

const char* const kLockOpen =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect width="18" height="11" x="3" y="11" rx="2" ry="2"/><path d="M7 11V7a5 5 0 0 1 9.9-1"/></svg>)";

const char* const kPlus =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14"/><path d="M12 5v14"/></svg>)";

// The toolbar's overflow menu and its entries.
const char* const kEllipsisVertical =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="1"/><circle cx="12" cy="5" r="1"/><circle cx="12" cy="19" r="1"/></svg>)";

const char* const kSquarePlus =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect width="18" height="18" x="3" y="3" rx="2"/><path d="M8 12h8"/><path d="M12 8v8"/></svg>)";

const char* const kMinus =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14"/></svg>)";

const char* const kSearch =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m21 21-4.34-4.34"/><circle cx="11" cy="11" r="8"/></svg>)";

const char* const kGlobe =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><path d="M12 2a14.5 14.5 0 0 0 0 20 14.5 14.5 0 0 0 0-20"/><path d="M2 12h20"/></svg>)";

const char* const kHistory =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"/><path d="M3 3v5h5"/><path d="M12 7v5l4 2"/></svg>)";

const char* const kTrash =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M10 11v6"/><path d="M14 11v6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/><path d="M3 6h18"/><path d="M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>)";

// The devtools console's four: clear, and the log/warn/error filter chips.
const char* const kBan =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><path d="M4.929 4.929 19.07 19.071"/></svg>)";

const char* const kScroll =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M19 17V5a2 2 0 0 0-2-2H4"/><path d="M8 21h12a2 2 0 0 0 2-2v-1a1 1 0 0 0-1-1H11a1 1 0 0 0-1 1v1a2 2 0 1 1-4 0V5a2 2 0 1 0-4 0v2a1 1 0 0 0 1 1h3"/></svg>)";

const char* const kTriangleAlert =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3"/><path d="M12 9v4"/><path d="M12 17h.01"/></svg>)";

const char* const kCircleX =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><path d="m15 9-6 6"/><path d="m9 9 6 6"/></svg>)";

const char* const kSquareDashedMousePointer =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12.034 12.681a.498.498 0 0 1 .647-.647l9 3.5a.5.5 0 0 1-.033.943l-3.444 1.068a1 1 0 0 0-.66.66l-1.067 3.443a.5.5 0 0 1-.943.033z"/><path d="M5 3a2 2 0 0 0-2 2"/><path d="M19 3a2 2 0 0 1 2 2"/><path d="M5 21a2 2 0 0 1-2-2"/><path d="M9 3h1"/><path d="M9 21h2"/><path d="M14 3h1"/><path d="M3 9v1"/><path d="M21 9v2"/><path d="M3 14v1"/></svg>)";

const char* const kChevronRight =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m9 18 6-6-6-6"/></svg>)";

// A round cap and a mitre reach past the viewBox edge, so the raster is grown by
// a unit on every side and the drawn box grows with it.
constexpr float kPadUnits = 1.0f;
constexpr float kBoxUnits = 24.0f + 2.0f * kPadUnits;

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    for (size_t i = s.find(from); i != std::string::npos; i = s.find(from, i + to.size()))
        s.replace(i, from.size(), to);
}

// nanosvg knows nothing of `currentColor`, and the stroke width is baked into
// the pixels, so both are patched into the source before it is parsed. White is
// what makes the tint at draw time a plain multiply.
std::string bake(const char* svg, float stroke_units) {
    std::string s(svg);
    replace_all(s, "currentColor", "#ffffff");
    // triangle-alert's dot is `M12 17h.01`, a segment so short that nanosvg
    // discards the second point as a duplicate and drops the subpath with it.
    // Lengthened to a third of a unit it survives and still rounds to a dot.
    replace_all(s, "h.01", "h.35");
    char buf[48];
    std::snprintf(buf, sizeof(buf), "stroke-width=\"%.4f\"", stroke_units);
    replace_all(s, "stroke-width=\"2\"", buf);
    return s;
}

struct IconTexture {
    unsigned int id = 0;
    int px = 0;         // square, in device pixels
};

// Keyed on the source, the device size and the stroke: the chrome asks for a
// handful of combinations and never releases them, which is why they are held
// for the process lifetime rather than reference-counted.
using IconKey = std::tuple<const char*, int, int>;
std::map<IconKey, IconTexture> g_cache;

const IconTexture* acquire(const char* svg, int px, float stroke_units) {
    IconKey key(svg, px, (int)std::lround(stroke_units * 100.0f));
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return it->second.id ? &it->second : nullptr;

    IconTexture tex;
    std::string src = bake(svg, stroke_units);
    std::vector<char> buf(src.begin(), src.end());
    buf.push_back('\0');

    NSVGimage* image = nsvgParse(buf.data(), "px", 96.0f);
    if (image && image->width > 0.0f) {
        NSVGrasterizer* rast = nsvgCreateRasterizer();
        if (rast) {
            const float scale = (float)px / kBoxUnits;
            std::vector<unsigned char> pixels((size_t)px * (size_t)px * 4, 0);
            nsvgRasterize(rast, image, kPadUnits * scale, kPadUnits * scale, scale,
                          pixels.data(), px, px, px * 4);
            nsvgDeleteRasterizer(rast);

            GLuint id = 0;
            glGenTextures(1, &id);
            glBindTexture(GL_TEXTURE_2D, id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#if defined(GL_UNPACK_ROW_LENGTH) && !defined(__EMSCRIPTEN__)
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, px, px, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            tex.id = id;
            tex.px = px;
        }
    }
    if (image) nsvgDelete(image);

    auto& slot = g_cache[key];
    slot = tex;
    return slot.id ? &slot : nullptr;
}

// `size` is the 24-unit viewBox's edge in logical pixels; `thickness` is the
// stroke in the same, with <= 0 meaning Lucide's own 2/24.
void DrawSvgIcon(const char* svg, ImVec2 center, ImU32 color, float size, float thickness) {
    if (size <= 0.0f) return;
    float fb = ImGui::GetIO().DisplayFramebufferScale.y;
    if (fb <= 0.0f) fb = 1.0f;

    const float stroke_units = thickness > 0.0f ? thickness * 24.0f / size : 2.0f;
    const int px = (int)std::lround(size * (kBoxUnits / 24.0f) * fb);
    const IconTexture* tex = acquire(svg, px, stroke_units);
    if (!tex) return;

    // Drawn at exactly the size it was rasterized at, so one texel is one device
    // pixel and the trace stays as crisp as nanosvg made it.
    const float draw = (float)tex->px / fb;
    ImVec2 min(std::round(center.x - draw * 0.5f), std::round(center.y - draw * 0.5f));
    ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)tex->id, min,
                                         ImVec2(min.x + draw, min.y + draw),
                                         ImVec2(0, 0), ImVec2(1, 1), color);
}

} // namespace

void DrawBackArrowIcon(ImVec2 center, ImU32 color, float size, float thickness) {
    DrawSvgIcon(kArrowLeft, center, color, size, thickness);
}

void DrawForwardArrowIcon(ImVec2 center, ImU32 color, float size, float thickness) {
    DrawSvgIcon(kArrowRight, center, color, size, thickness);
}

void DrawReloadIcon(ImVec2 center, ImU32 color, float size, float thickness) {
    DrawSvgIcon(kRotateCw, center, color, size, thickness);
}

void DrawPlusIcon(ImVec2 center, ImU32 color, float size, float thickness) {
    DrawSvgIcon(kPlus, center, color, size, thickness);
}

void DrawXIcon(ImVec2 center, ImU32 color, float size, float thickness) {
    DrawSvgIcon(kX, center, color, size, thickness);
}

void DrawLockIcon(ImVec2 center, ImU32 color, bool closed, float size, float thickness) {
    DrawSvgIcon(closed ? kLock : kLockOpen, center, color, size, thickness);
}

void DrawMenuIcon(ImVec2 center, ImU32 color, float size, float thickness) {
    DrawSvgIcon(kEllipsisVertical, center, color, size, thickness);
}

void DrawNewTabIcon(ImVec2 center, ImU32 color, float size, float thickness) {
    DrawSvgIcon(kSquarePlus, center, color, size, thickness);
}

void DrawHistoryIcon(ImVec2 center, ImU32 color, float size, float thickness) {
    DrawSvgIcon(kHistory, center, color, size, thickness);
}

void DrawTrashIcon(ImVec2 center, ImU32 color, float size, float thickness) {
    DrawSvgIcon(kTrash, center, color, size, thickness);
}

void DrawMinusIcon(ImVec2 center, ImU32 color, float size, float thickness) {
    DrawSvgIcon(kMinus, center, color, size, thickness);
}

void DrawZoomIcon(ImVec2 center, ImU32 color, float size, float thickness) {
    DrawSvgIcon(kSearch, center, color, size, thickness);
}

void DrawGlobeIcon(ImVec2 center, ImU32 color, float size, float thickness) {
    DrawSvgIcon(kGlobe, center, color, size, thickness);
}

// The chrome's stroke weight, not Lucide's own 2/24.
constexpr float kDtStroke = 1.25f;

void DrawBanIcon(ImVec2 center, ImU32 color, float size) {
    DrawSvgIcon(kBan, center, color, size, kDtStroke);
}

void DrawScrollIcon(ImVec2 center, ImU32 color, float size) {
    DrawSvgIcon(kScroll, center, color, size, kDtStroke);
}

void DrawTriangleAlertIcon(ImVec2 center, ImU32 color, float size) {
    DrawSvgIcon(kTriangleAlert, center, color, size, kDtStroke);
}

void DrawCircleXIcon(ImVec2 center, ImU32 color, float size) {
    DrawSvgIcon(kCircleX, center, color, size, kDtStroke);
}

void DrawInspectIcon(ImVec2 center, ImU32 color, float size) {
    DrawSvgIcon(kSquareDashedMousePointer, center, color, size, kDtStroke);
}

void DrawChevronRightIcon(ImVec2 center, ImU32 color, float size) {
    DrawSvgIcon(kChevronRight, center, color, size, kDtStroke);
}
