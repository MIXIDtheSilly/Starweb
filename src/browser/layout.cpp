#include "layout.hpp"
#include "parser.hpp"
#include "renderer.hpp"
#include "globals.hpp"
#include "fetcher.hpp"
#include "imgui.h"
#include <yoga/Yoga.h>
#include <algorithm>
#include <string>

static YGFlexDirection map_direction(const std::string& s) {
    if (s == "column") return YGFlexDirectionColumn;
    if (s == "column-reverse") return YGFlexDirectionColumnReverse;
    if (s == "row-reverse") return YGFlexDirectionRowReverse;
    return YGFlexDirectionRow;
}

static YGJustify map_justify(const std::string& s) {
    if (s == "center") return YGJustifyCenter;
    if (s == "flex-end" || s == "end") return YGJustifyFlexEnd;
    if (s == "space-between") return YGJustifySpaceBetween;
    if (s == "space-around") return YGJustifySpaceAround;
    if (s == "space-evenly") return YGJustifySpaceEvenly;
    return YGJustifyFlexStart;
}

static YGAlign map_align(const std::string& s, YGAlign fallback) {
    if (s == "auto") return YGAlignAuto;
    if (s == "flex-start" || s == "start") return YGAlignFlexStart;
    if (s == "center") return YGAlignCenter;
    if (s == "flex-end" || s == "end") return YGAlignFlexEnd;
    if (s == "stretch") return YGAlignStretch;
    if (s == "baseline") return YGAlignBaseline;
    return fallback;
}

static YGWrap map_wrap(const std::string& s) {
    if (s == "wrap") return YGWrapWrap;
    if (s == "wrap-reverse") return YGWrapWrapReverse;
    return YGWrapNoWrap;
}

struct MeasureCtx {
    const DomNode* node;
    CssStyle merged;
    Tab* tab;
};

static float header_scale(const std::string& tag) {
    if (tag == "h1") return 1.8f;
    if (tag == "h2") return 1.4f;
    if (tag == "h3") return 1.2f;
    if (tag == "h4") return 1.1f;
    if (tag == "h6") return 0.9f;
    return 1.0f;
}

static bool is_renderable(const std::string& tag) {
    return !(tag == "script" || tag == "style" || tag == "head" ||
             tag == "title" || tag == "meta" || tag == "option");
}

static bool is_text_tag(const std::string& t) {
    return t == "p" || t == "span" || t == "a" || t == "li" || t == "#text" ||
           t == "h1" || t == "h2" || t == "h3" || t == "h4" || t == "h5" || t == "h6" ||
           t == "b" || t == "strong" || t == "i" || t == "em" || t == "u" ||
           t == "code" || t == "mark" || t == "small" || t == "del" || t == "s" ||
           t == "strike" || t == "ins" || t == "label" || t == "cite" || t == "q" ||
           t == "abbr" || t == "kbd" || t == "samp" || t == "var" || t == "sub" || t == "sup";
}

// Estimated content size for a node given the width it may occupy, used to drive
// Yoga's measure callback.
static ImVec2 measure_intrinsic(const DomNode& node, const CssStyle& merged, float avail_w, Tab& tab) {
    const std::string& tag = node.tag;
    const float pad_x = merged.padding_left + merged.padding_right;
    const float pad_y = merged.padding_top + merged.padding_bottom;
    if (avail_w < 1.0f) avail_w = 1.0f;

    if (tag == "img") {
        std::string src = node.src;
        if (src.find("://") == std::string::npos) src = resolve_url(tab.current_url, src);
        float natW = 0.0f, natH = 0.0f;
        auto it = tab.page_textures.find(src);
        if (it != tab.page_textures.end() && it->second.id != 0) {
            natW = (float)it->second.width;
            natH = (float)it->second.height;
        }
        float w = merged.width > 0.0f ? merged.width : (natW > 0.0f ? natW : 100.0f);
        float h = merged.height > 0.0f ? merged.height : (natH > 0.0f ? natH : 100.0f);
        if (w > avail_w) { h *= avail_w / w; w = avail_w; }
        return ImVec2(w, h);
    }
    if (tag == "video") {
        float w = merged.width > 0.0f ? merged.width : 500.0f;
        float h = merged.height > 0.0f ? merged.height : 375.0f;
        if (w > avail_w) { h *= avail_w / w; w = avail_w; }
        return ImVec2(w, h);
    }
    if (tag == "audio") {
        return ImVec2(merged.width > 0.0f ? merged.width : 450.0f, 42.0f);
    }
    if (tag == "button") {
        std::string t = collapse_whitespace(node.text_content);
        ImFont* fam = font_for_family(merged.font_family);
        if (fam) ImGui::PushFont(fam);
        ImVec2 ts = ImGui::CalcTextSize(t.c_str());
        if (fam) ImGui::PopFont();
        return ImVec2(merged.width > 0.0f ? merged.width : ts.x + 36.0f,
                      merged.height > 0.0f ? merged.height : ts.y + 12.0f);
    }
    if (tag == "input" || tag == "select") {
        // A field is drawn from its own padding (see the field guards in
        // renderer.cpp), not from the global frame padding, so GetFrameHeight()
        // would hand the layout a shorter box than the one that gets painted and
        // leave the field sitting low in a centred flex row.
        float field_pad_y = merged.padding_top > 0.0f ? merged.padding_top : 2.0f;
        float line = ImGui::GetFontSize() * merged.font_size;
        return ImVec2(merged.width > 0.0f ? merged.width : (tag == "select" ? 150.0f : 200.0f),
                      merged.height > 0.0f ? merged.height : line + field_pad_y * 2.0f);
    }
    if (tag == "textarea") {
        return ImVec2(merged.width > 0.0f ? merged.width : 300.0f,
                      merged.height > 0.0f ? merged.height : 100.0f);
    }
    if (tag == "hr") {
        return ImVec2(merged.width > 0.0f ? merged.width : avail_w, 8.0f);
    }

    std::string text = collapse_whitespace(node.text_content);
    bool has_elem_child = false;
    for (const auto& c : node.children) {
        if (c.tag != "#text" && is_renderable(c.tag)) { has_elem_child = true; break; }
    }

    if (is_text_tag(tag) && !text.empty() && !has_elem_child) {
        float scale = merged.font_size * header_scale(tag);
        if (tag == "small" || tag == "sub" || tag == "sup") scale = merged.font_size * 0.8f;
        bool mono = (tag == "code" || tag == "kbd" || tag == "samp" || tag == "var") && mono_font != nullptr;
        ImFont* fam = mono ? mono_font : font_for_family(merged.font_family);
        if (fam) ImGui::PushFont(fam);
        ImGui::SetWindowFontScale(scale);
        float box_w = merged.width > 0.0f ? merged.width : avail_w;
        float wrap = std::max(1.0f, box_w - pad_x);
        ImVec2 ts = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap);
        ImGui::SetWindowFontScale(1.0f);
        if (fam) ImGui::PopFont();
        float w = merged.width > 0.0f ? merged.width : ts.x + pad_x;
        float h = merged.height > 0.0f ? merged.height : ts.y + pad_y;
        if (tag == "p" || tag[0] == 'h') h += ImGui::GetTextLineHeightWithSpacing() * 0.3f;
        return ImVec2(w, h);
    }

    // Nested container: stack (or lay out) its children to estimate its size.
    bool child_row = merged.display == "flex" && (merged.flex_direction.empty() ||
                     merged.flex_direction == "row" || merged.flex_direction == "row-reverse");
    float col_gap = std::max(0.0f, merged.column_gap);
    float row_gap = std::max(0.0f, merged.row_gap);
    float inner_avail = std::max(1.0f, (merged.width > 0.0f ? merged.width : avail_w) - pad_x);

    float main = 0.0f, cross = 0.0f;
    int count = 0;
    for (const auto& child : node.children) {
        if (!is_renderable(child.tag)) continue;
        if (child.tag == "#text" && collapse_whitespace(child.text_content).empty()) continue;
        CssStyle cm = merge_node_style(child, merged, tab);
        ImVec2 cs = measure_intrinsic(child, cm, inner_avail, tab);
        float cw = cs.x + cm.margin_left + cm.margin_right;
        float ch = cs.y + cm.margin_top + cm.margin_bottom;
        if (child_row) {
            main += cw + (count > 0 ? col_gap : 0.0f);
            cross = std::max(cross, ch);
        } else {
            main += ch + (count > 0 ? row_gap : 0.0f);
            cross = std::max(cross, cw);
        }
        count++;
    }
    float w = merged.width > 0.0f ? merged.width : (child_row ? main : cross) + pad_x;
    float h = merged.height > 0.0f ? merged.height : (child_row ? cross : main) + pad_y;
    return ImVec2(w, h);
}

static YGSize measure_cb(YGNodeConstRef node, float width, YGMeasureMode wmode,
                         float height, YGMeasureMode hmode) {
    MeasureCtx* ctx = (MeasureCtx*)YGNodeGetContext(node);
    float avail = (wmode == YGMeasureModeUndefined)
        ? (ctx->merged.width > 0.0f ? ctx->merged.width : 100000.0f)
        : width;
    ImVec2 s = measure_intrinsic(*ctx->node, ctx->merged, avail, *ctx->tab);

    YGSize out;
    out.width  = (wmode == YGMeasureModeExactly) ? width
               : (wmode == YGMeasureModeAtMost)  ? std::min(s.x, width) : s.x;
    out.height = (hmode == YGMeasureModeExactly) ? height
               : (hmode == YGMeasureModeAtMost)  ? std::min(s.y, height) : s.y;
    return out;
}

void compute_flex_layout(DomNode& container,
                         const CssStyle& container_style,
                         float content_width,
                         Tab& tab,
                         std::vector<DomNode*>& out_children,
                         std::vector<FlexRect>& out_rects,
                         float& out_width,
                         float& out_height) {
    std::vector<MeasureCtx*> ctxs;
    if (content_width < 1.0f) content_width = 1.0f;

    YGConfigRef cfg = YGConfigNew();
    YGConfigSetUseWebDefaults(cfg, true);

    YGNodeRef root = YGNodeNewWithConfig(cfg);
    YGNodeStyleSetFlexDirection(root, map_direction(container_style.flex_direction));
    YGNodeStyleSetJustifyContent(root, map_justify(container_style.justify_content));
    if (!container_style.align_items.empty())
        YGNodeStyleSetAlignItems(root, map_align(container_style.align_items, YGAlignStretch));
    YGNodeStyleSetFlexWrap(root, map_wrap(container_style.flex_wrap));
    if (container_style.column_gap >= 0.0f) YGNodeStyleSetGap(root, YGGutterColumn, container_style.column_gap);
    if (container_style.row_gap >= 0.0f)    YGNodeStyleSetGap(root, YGGutterRow, container_style.row_gap);
    YGNodeStyleSetWidth(root, content_width);
    if (container_style.height > 0.0f) YGNodeStyleSetHeight(root, container_style.height);

    // A positioned child still renders, at its own offset, but takes no part in
    // the layout: -1 marks it as having no Yoga node to read a rect back from.
    std::vector<int> yoga_index;

    uint32_t idx = 0;
    for (auto& child : container.children) {
        if (!is_renderable(child.tag)) continue;
        if (child.tag == "#text" && collapse_whitespace(child.text_content).empty()) continue;

        CssStyle cm = merge_node_style(child, container_style, tab);
        if (is_positioned(cm)) {
            out_children.push_back(&child);
            yoga_index.push_back(-1);
            continue;
        }

        YGNodeRef c = YGNodeNewWithConfig(cfg);

        if (cm.margin_left > 0.0f)   YGNodeStyleSetMargin(c, YGEdgeLeft, cm.margin_left);
        if (cm.margin_right > 0.0f)  YGNodeStyleSetMargin(c, YGEdgeRight, cm.margin_right);
        if (cm.margin_top > 0.0f)    YGNodeStyleSetMargin(c, YGEdgeTop, cm.margin_top);
        if (cm.margin_bottom > 0.0f) YGNodeStyleSetMargin(c, YGEdgeBottom, cm.margin_bottom);

        if (cm.width > 0.0f)  YGNodeStyleSetWidth(c, cm.width);
        if (cm.height > 0.0f) YGNodeStyleSetHeight(c, cm.height);

        if (cm.flex_grow >= 0.0f)   YGNodeStyleSetFlexGrow(c, cm.flex_grow);
        if (cm.flex_shrink >= 0.0f) YGNodeStyleSetFlexShrink(c, cm.flex_shrink);
        if (cm.flex_basis >= 0.0f)  YGNodeStyleSetFlexBasis(c, cm.flex_basis);
        if (!cm.align_self.empty()) YGNodeStyleSetAlignSelf(c, map_align(cm.align_self, YGAlignAuto));

        if (cm.width <= 0.0f || cm.height <= 0.0f) {
            MeasureCtx* ctx = new MeasureCtx{&child, cm, &tab};
            ctxs.push_back(ctx);
            YGNodeSetContext(c, ctx);
            YGNodeSetMeasureFunc(c, measure_cb);
        }

        YGNodeInsertChild(root, c, idx);
        out_children.push_back(&child);
        yoga_index.push_back((int)idx++);
    }

    YGNodeCalculateLayout(root, content_width, YGUndefined, YGDirectionLTR);

    out_rects.reserve(yoga_index.size());
    for (int gi : yoga_index) {
        FlexRect r;
        if (gi >= 0) {
            YGNodeRef c = YGNodeGetChild(root, (uint32_t)gi);
            r.x = YGNodeLayoutGetLeft(c);
            r.y = YGNodeLayoutGetTop(c);
            r.w = YGNodeLayoutGetWidth(c);
            r.h = YGNodeLayoutGetHeight(c);
        }
        out_rects.push_back(r);
    }
    out_width  = YGNodeLayoutGetWidth(root);
    out_height = YGNodeLayoutGetHeight(root);

    YGNodeFreeRecursive(root);
    YGConfigFree(cfg);
    for (auto* ctx : ctxs) delete ctx;
}
