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
#include <filesystem>
#include <fstream>

#define STB_IMAGE_IMPLEMENTATION
#include "../thirdparty/stb_image.h"

bool LoadTextureFromMemory(const unsigned char* image_data, int image_size, unsigned int* out_texture, int* out_width, int* out_height) {
    int image_width = 0;
    int image_height = 0;
    unsigned char* data = stbi_load_from_memory(image_data, image_size, &image_width, &image_height, NULL, 4);
    if (data == NULL) return false;

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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image_width, image_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    stbi_image_free(data);

    *out_texture = image_texture;
    *out_width = image_width;
    *out_height = image_height;

    return true;
}

int main() {
    net::Startup net_startup;
    std::filesystem::create_directories("cache");
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
    GLFWwindow* window = glfwCreateWindow(1024, 768, "Starmap", nullptr, nullptr);
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

    ImGui::StyleColorsDark();

    auto& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.ChildRounding = 6.0f;

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
    tabs.push_back(initial_tab);
    active_tab_idx = 0;
    start_async_fetch(tabs[active_tab_idx].id, tabs[active_tab_idx].current_url);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            for (size_t idx = 0; idx < tabs.size(); idx++) {
                auto& tab = tabs[idx];
                if (tab.new_page_ready) {
                    tab.is_fetching = false;
                    tab.new_page_ready = false;
                    tab.reset_scroll_next_frame = true;
                    
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
                        for (const auto& [url, bytes] : tab.active_page.fetched_media) {
                            std::string cache_path = get_cache_filepath(url);
                            std::ofstream outfile(cache_path, std::ios::binary);
                            if (outfile) {
                                outfile.write(bytes.data(), bytes.size());
                            }
                        }
                        // Load new textures on the main thread
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
                        
                        tab.alert_text = extract_alert_message(tab.active_page.body);
                    } else {
                        tab.status_text = "Error: " + tab.active_page.error_message;
                        std::string error_html = "<h1>Error loading page</h1><p>" + tab.active_page.error_message + "</p>";
                        std::string temp_css = "";
                        tab.page_dom = parse_html_to_dom(error_html, temp_css);
                        tab.css_classes.clear();
                        tab.title = "Error Loading";
                        tab.alert_text = "";
                    }
                    
                    if (idx == (size_t)active_tab_idx) {
                        glfwSetWindowTitle(window, ("Starmap - " + tab.title).c_str());
                    }
                }
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

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
        ImGui::Begin("StarmapWorkspace", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
        
        float tab_height = 34.0f;
        float max_tab_width = 180.0f;
        float min_tab_width = 36.0f;
        
        float window_avail_width = ImGui::GetContentRegionAvail().x;
        float avail_w = window_avail_width - 140.0f; 
        float tab_width = avail_w / tabs.size();
        if (tab_width > max_tab_width) tab_width = max_tab_width;
        if (tab_width < min_tab_width) tab_width = min_tab_width;

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
        
        ImVec2 bar_min = cursor_pos;
        ImVec2 bar_max = ImVec2(bar_min.x + window_avail_width, bar_min.y + tab_height);
        draw_list->AddRectFilled(bar_min, bar_max, Theme::bar_bg);

        // Draw Custom macOS Traffic Lights
        ImVec2 tl_pos = cursor_pos;
        ImVec2 mouse_pos = ImGui::GetIO().MousePos;
        bool mouse_clicked = ImGui::IsMouseClicked(0);
        
        bool red_hovered = (mouse_pos.x >= tl_pos.x + 12.0f && mouse_pos.x < tl_pos.x + 28.0f &&
                            mouse_pos.y >= tl_pos.y + 9.0f && mouse_pos.y < tl_pos.y + 25.0f);
        bool yellow_hovered = (mouse_pos.x >= tl_pos.x + 32.0f && mouse_pos.x < tl_pos.x + 48.0f &&
                               mouse_pos.y >= tl_pos.y + 9.0f && mouse_pos.y < tl_pos.y + 25.0f);
        bool green_hovered = (mouse_pos.x >= tl_pos.x + 52.0f && mouse_pos.x < tl_pos.x + 68.0f &&
                              mouse_pos.y >= tl_pos.y + 9.0f && mouse_pos.y < tl_pos.y + 25.0f);

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

        draw_list->AddCircleFilled(ImVec2(tl_pos.x + 20.0f, tl_pos.y + 17.0f), 6.0f, red_hovered ? IM_COL32(255, 70, 70, 255) : IM_COL32(255, 95, 87, 255));
        draw_list->AddCircleFilled(ImVec2(tl_pos.x + 40.0f, tl_pos.y + 17.0f), 6.0f, yellow_hovered ? IM_COL32(240, 170, 30, 255) : IM_COL32(255, 189, 46, 255));
        draw_list->AddCircleFilled(ImVec2(tl_pos.x + 60.0f, tl_pos.y + 17.0f), 6.0f, green_hovered ? IM_COL32(30, 180, 50, 255) : IM_COL32(40, 201, 64, 255));

        ImVec2 clip_min = ImVec2(cursor_pos.x + 80.0f, cursor_pos.y);
        ImVec2 clip_max = ImVec2(cursor_pos.x + window_avail_width - 36.0f, cursor_pos.y + tab_height);
        draw_list->PushClipRect(clip_min, clip_max, true);

        int tab_to_close = -1;
        int tab_to_select = -1;

        for (size_t i = 0; i < tabs.size(); ++i) {
            auto& tab = tabs[i];
            bool is_active = ((int)i == active_tab_idx);
            
            ImVec2 tab_min = ImVec2(cursor_pos.x + i * tab_width + 80.0f, cursor_pos.y);
            ImVec2 tab_max = ImVec2(tab_min.x + tab_width, tab_min.y + tab_height);
            bool tab_hovered = (mouse_pos.x >= tab_min.x && mouse_pos.x < tab_max.x &&
                                mouse_pos.y >= tab_min.y && mouse_pos.y < tab_max.y);
            
            bool show_close = (tab_width >= 50.0f) || tab_hovered;
            bool close_hovered = show_close && tab_hovered && (mouse_pos.x >= tab_max.x - 28.0f);
            bool click_hovered = tab_hovered && !close_hovered;

            if (mouse_clicked) {
                if (close_hovered) {
                    tab_to_close = (int)i;
                } else if (click_hovered) {
                    tab_to_select = (int)i;
                }
            }

            float rounding = 6.0f;
            ImU32 tab_bg_col;
            if (is_active) {
                tab_bg_col = Theme::tab_active_bg;
            } else if (tab_hovered) {
                tab_bg_col = Theme::tab_hover_bg;
            } else {
                tab_bg_col = Theme::tab_inactive_bg;
            }

            draw_list->AddRectFilled(tab_min, tab_max, tab_bg_col, rounding, ImDrawFlags_RoundCornersTop);

            if (is_active) {
                draw_list->AddRectFilled(ImVec2(tab_min.x, tab_min.y), ImVec2(tab_max.x, tab_min.y + 2.0f), Theme::tab_accent_stripe);
            }

            if (!is_active && i < tabs.size() - 1 && (int)i + 1 != active_tab_idx) {
                draw_list->AddLine(
                    ImVec2(tab_max.x, tab_min.y + 7.0f), 
                    ImVec2(tab_max.x, tab_max.y - 7.0f), 
                    Theme::tab_divider, 
                    1.0f
                );
            }

            float text_center_y = std::round(tab_min.y + tab_height * 0.5f);
            if (tab_width >= 60.0f) {
                float text_y = std::round(text_center_y - ImGui::GetFontSize() * 0.5f);
                ImVec2 text_min = ImVec2(tab_min.x + 10.0f, text_y);
                ImVec2 text_max = ImVec2(tab_max.x - 42.0f, text_y + ImGui::GetFontSize());
                draw_list->PushClipRect(text_min, text_max, true);
                draw_list->AddText(text_min, is_active ? IM_COL32(240, 240, 240, 255) : IM_COL32(170, 170, 180, 255), tab.title.c_str());
                draw_list->PopClipRect();

                ImVec2 mask_min = ImVec2(text_max.x - 20.0f, text_min.y);
                ImVec2 mask_max = ImVec2(text_max.x, text_max.y);
                ImU32 transparent_bg = tab_bg_col & 0x00FFFFFF;
                draw_list->AddRectFilledMultiColor(
                    mask_min, mask_max,
                    transparent_bg, tab_bg_col,
                    tab_bg_col, transparent_bg
                );
            }

            if (show_close) {
                ImVec2 x_center = ImVec2(std::round(tab_max.x - 14.0f), text_center_y);
                float x_size = 6.0f;
                float half_size = x_size * 0.5f;
                
                ImU32 x_color = close_hovered ? IM_COL32(240, 240, 240, 255) : (is_active ? IM_COL32(180, 180, 190, 255) : IM_COL32(120, 120, 130, 255));
                if (close_hovered) {
                    draw_list->AddCircleFilled(x_center, 7.0f, IM_COL32(120, 120, 120, 70));
                }
                draw_list->AddLine(ImVec2(x_center.x - half_size, x_center.y - half_size), ImVec2(x_center.x + half_size, x_center.y + half_size), x_color, 1.5f);
                draw_list->AddLine(ImVec2(x_center.x - half_size, x_center.y + half_size), ImVec2(x_center.x + half_size, x_center.y - half_size), x_color, 1.5f);
            }
        }

        draw_list->PopClipRect();

        float max_plus_x = cursor_pos.x + window_avail_width - 36.0f;
        float plus_x = cursor_pos.x + tabs.size() * tab_width + 80.0f + 8.0f;
        if (plus_x > max_plus_x) plus_x = max_plus_x;

        ImVec2 plus_min = ImVec2(plus_x, cursor_pos.y + 6.0f);
        ImVec2 plus_max = ImVec2(plus_min.x + 22.0f, plus_min.y + 22.0f);
        
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
        
        ImU32 plus_bg = plus_active ? Theme::plus_bg_active : (plus_hovered ? Theme::plus_bg_hover : Theme::plus_bg_normal);
        draw_list->AddRectFilled(plus_min, plus_max, plus_bg, 4.0f);
        
        ImVec2 plus_center = ImVec2((plus_min.x + plus_max.x) * 0.5f, (plus_min.y + plus_max.y) * 0.5f);
        ImU32 plus_color = plus_hovered ? Theme::plus_color_hover : Theme::plus_color_normal;
        draw_list->AddLine(ImVec2(plus_center.x - 5.0f, plus_center.y), ImVec2(plus_center.x + 5.0f, plus_center.y), plus_color, 1.5f);
        draw_list->AddLine(ImVec2(plus_center.x, plus_center.y - 5.0f), ImVec2(plus_center.x, plus_center.y + 5.0f), plus_color, 1.5f);

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
            
            for (auto& [url, player] : tabs[tab_to_close].active_players) {
                delete player;
            }
            tabs[tab_to_close].active_players.clear();
            
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

        float active_tab_min_x = cursor_pos.x + active_tab_idx * tab_width + 80.0f;
        float active_tab_max_x = active_tab_min_x + tab_width;
        
        draw_list->AddLine(
            ImVec2(cursor_pos.x, cursor_pos.y + tab_height), 
            ImVec2(active_tab_min_x, cursor_pos.y + tab_height), 
            Theme::border_separator,
            1.5f
        );
        draw_list->AddLine(
            ImVec2(active_tab_max_x, cursor_pos.y + tab_height), 
            ImVec2(cursor_pos.x + window_avail_width, cursor_pos.y + tab_height), 
            Theme::border_separator,
            1.5f
        );

        static bool is_dragging = false;
        static double drag_start_x = 0;
        static double drag_start_y = 0;
        if (ImGui::IsMouseClicked(0)) {
            ImVec2 m_pos = ImGui::GetIO().MousePos;
            ImVec2 w_pos = ImGui::GetWindowPos();
            if (m_pos.y >= w_pos.y + 6.0f && m_pos.y <= w_pos.y + tab_height && m_pos.x >= w_pos.x && m_pos.x < w_pos.x + window_avail_width) {
                bool over_interactive = false;
                if (m_pos.x < w_pos.x + 80.0f) over_interactive = true;
                
                for (size_t i = 0; i < tabs.size(); ++i) {
                    float t_min_x = w_pos.x + 80.0f + i * tab_width;
                    float t_max_x = t_min_x + tab_width;
                    if (m_pos.x >= t_min_x && m_pos.x <= t_max_x) {
                        over_interactive = true;
                        break;
                    }
                }
                
                float plus_start_x = w_pos.x + 80.0f + tabs.size() * tab_width + 8.0f;
                if (m_pos.x >= plus_start_x && m_pos.x <= plus_start_x + 30.0f) {
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

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));
        
        float btn_size = ImGui::GetFrameHeight();
        float toolbar_height = btn_size + 16.0f;
        
        ImVec2 toolbar_min = ImVec2(cursor_pos.x, cursor_pos.y + tab_height);
        ImVec2 toolbar_max = ImVec2(toolbar_min.x + window_avail_width, toolbar_min.y + toolbar_height);
        draw_list->AddRectFilled(toolbar_min, toolbar_max, Theme::toolbar_bg);
        
        draw_list->AddLine(
            ImVec2(toolbar_min.x, toolbar_max.y),
            ImVec2(toolbar_max.x, toolbar_max.y),
            Theme::border_separator,
            1.0f
        );

        ImGui::SetCursorScreenPos(ImVec2(toolbar_min.x + 8.0f, toolbar_min.y + 8.0f));

        ImGui::BeginGroup();
        
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::btn_hover_highlight);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::btn_active_highlight);
        
        float rounding = btn_size * 0.5f;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
        
        bool back_disabled = (active_tab.history_index <= 0);
        ImGui::BeginDisabled(back_disabled);
        bool back_clicked = ImGui::Button("##back", ImVec2(btn_size, btn_size));
        ImVec2 back_center = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                                    (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
        ImU32 back_color = back_disabled ? IM_COL32(75, 75, 75, 255) : ImGui::GetColorU32(ImGuiCol_Text);
        DrawBackArrowIcon(back_center, back_color);
        if (back_clicked) {
            active_tab.history_index--;
            start_async_fetch(active_tab.id, active_tab.navigation_history[active_tab.history_index], true);
        }
        ImGui::EndDisabled();
        
        ImGui::SameLine();
        
        bool forward_disabled = (active_tab.history_index >= (int)active_tab.navigation_history.size() - 1 || active_tab.navigation_history.empty());
        ImGui::BeginDisabled(forward_disabled);
        bool forward_clicked = ImGui::Button("##forward", ImVec2(btn_size, btn_size));
        ImVec2 forward_center = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                                       (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
        ImU32 forward_color = forward_disabled ? IM_COL32(75, 75, 75, 255) : ImGui::GetColorU32(ImGuiCol_Text);
        DrawForwardArrowIcon(forward_center, forward_color);
        if (forward_clicked) {
            active_tab.history_index++;
            start_async_fetch(active_tab.id, active_tab.navigation_history[active_tab.history_index], true);
        }
        ImGui::EndDisabled();
        
        ImGui::SameLine();
        
        if (active_tab.is_fetching) {
            ImGui::Dummy(ImVec2(btn_size, btn_size));
            ImVec2 spinner_center = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                                           (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
            DrawSpinner(spinner_center, 6.0f, 2.0f, Theme::spinner);
        } else {
            bool reload_clicked = ImGui::Button("##reload", ImVec2(btn_size, btn_size));
            ImVec2 reload_center = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                                          (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
            DrawReloadIcon(reload_center, 6.0f, ImGui::GetColorU32(ImGuiCol_Text));
            if (reload_clicked) {
                start_async_fetch(active_tab.id, active_tab.current_url);
            }
        }
        
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        
        ImGui::SameLine();
        
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Theme::omnibox_bg);
        
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 8.0f);
        if (ImGui::InputText("##url", active_tab.url_input, IM_ARRAYSIZE(active_tab.url_input), ImGuiInputTextFlags_EnterReturnsTrue)) {
            start_async_fetch(active_tab.id, active_tab.url_input);
        }
        ImGui::PopItemWidth();
        
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        
        ImGui::EndGroup();
        
        ImGui::PopStyleVar();
        
        ImGui::SetCursorScreenPos(ImVec2(toolbar_min.x, toolbar_max.y + 4.0f));

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_InputTextCursor, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
        ImGui::BeginChild("RenderViewport", ImVec2(0, 0), false, 0);
        if (active_tab.reset_scroll_next_frame) {
            ImGui::SetScrollY(0.0f);
            active_tab.reset_scroll_next_frame = false;
        }
        
        auto body_it = active_tab.css_classes.find("body");
        ImDrawList* vp_draw_list = ImGui::GetWindowDrawList();
        ImVec2 min_p = ImGui::GetWindowPos();
        ImVec2 max_p = ImVec2(min_p.x + ImGui::GetWindowWidth(), min_p.y + ImGui::GetWindowHeight());
        
        float inner_radius = ImGui::GetStyle().ChildRounding;
        
        if (body_it != active_tab.css_classes.end()) {
            const auto& body_style = body_it->second;
            if (body_style.has_gradient) {
                ImU32 col_start = ImGui::ColorConvertFloat4ToU32(body_style.gradient_start);
                ImU32 col_end = ImGui::ColorConvertFloat4ToU32(body_style.gradient_end);
                vp_draw_list->AddRectFilledMultiColor(min_p, max_p, col_start, col_start, col_end, col_end);
            } else if (body_style.has_bg) {
                vp_draw_list->AddRectFilled(min_p, max_p, ImGui::ColorConvertFloat4ToU32(body_style.bg_color), inner_radius, ImDrawFlags_RoundCornersBottom);
            } else {
                vp_draw_list->AddRectFilled(min_p, max_p, Theme::viewport_bg, inner_radius, ImDrawFlags_RoundCornersBottom);
            }
        } else {
            vp_draw_list->AddRectFilled(min_p, max_p, Theme::viewport_bg, inner_radius, ImDrawFlags_RoundCornersBottom);
        }

        CssStyle default_style;
        default_style.color = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);
        default_style.has_color = true;
        bool default_inline_flow = false;
        render_node(active_tab.page_dom, default_style, default_inline_flow, active_tab);

        ImGui::EndChild();
        ImGui::PopStyleColor(2);

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

        ImGui::Render();
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.07f, 0.09f, 0.15f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    for (auto& tab : tabs) {
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
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}