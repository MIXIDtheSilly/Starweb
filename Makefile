# Cross-platform Makefile for macOS and Linux.
#
#   macOS : media backend = AVFoundation      (needs: glfw)
#   Linux : media backend = FFmpeg + miniaudio (needs: glfw, ffmpeg dev libs)
#
# On Windows, use CMake instead (this Makefile relies on a POSIX shell):
#   cmake -S . -B build && cmake --build build --config Release
# CMake also works on macOS/Linux if you prefer it over make.

UNAME_S := $(shell uname -s)

CXX = clang++
CC = cc
CXXFLAGS = -std=c++20 -Wall -Wextra -O3 -pthread

IMGUI_DIR = src/thirdparty/imgui
IMGUI_INC = -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends

COMMON_HDRS = $(wildcard src/common/*.hpp)
BROWSER_HDRS = $(wildcard src/browser/*.hpp)

LUA_DIR = src/thirdparty/lua
LUA_INC = -I$(LUA_DIR)
LUA_SRCS = $(wildcard $(LUA_DIR)/*.c)
LUA_OBJS = $(patsubst $(LUA_DIR)/%.c,$(OBJ_DIR)/lua/%.o,$(LUA_SRCS))

YOGA_DIR = src/thirdparty/yoga
YOGA_INC = -I$(YOGA_DIR) -include src/thirdparty/yoga_compat.hpp
YOGA_SRCS = $(shell find $(YOGA_DIR)/yoga -name '*.cpp')

GLFW_CFLAGS = $(shell pkg-config --cflags glfw3 2>/dev/null || echo "")
GLFW_LIBS = $(shell pkg-config --libs glfw3 2>/dev/null || echo "-lglfw")

# brew keeps openssl@3 keg-only, so nudge pkg-config toward it if the bare
# lookup misses. Falls back to plain -lssl -lcrypto if pkg-config has nothing.
export PKG_CONFIG_PATH := /usr/local/opt/openssl@3/lib/pkgconfig:/opt/homebrew/opt/openssl@3/lib/pkgconfig:$(PKG_CONFIG_PATH)
SSL_CFLAGS = $(shell pkg-config --cflags openssl 2>/dev/null || echo "")
SSL_LIBS = $(shell pkg-config --libs openssl 2>/dev/null || echo "-lssl -lcrypto")

OBJ_DIR = obj

IMGUI_OBJS = $(OBJ_DIR)/imgui.o \
             $(OBJ_DIR)/imgui_draw.o \
             $(OBJ_DIR)/imgui_widgets.o \
             $(OBJ_DIR)/imgui_tables.o \
             $(OBJ_DIR)/imgui_impl_glfw.o \
             $(OBJ_DIR)/imgui_impl_opengl3.o

YOGA_OBJS = $(patsubst $(YOGA_DIR)/%.cpp,$(OBJ_DIR)/yoga/%.o,$(YOGA_SRCS))

ifeq ($(UNAME_S),Darwin)
    MEDIA_SRCS = src/browser/media_player_mac.mm
    MEDIA_FLAGS = -fobjc-arc
    MEDIA_LIBS = -framework AVFoundation -framework CoreMedia -framework AudioToolbox -framework QuartzCore
    GL_LIBS = -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
else
    CXX = g++
    MEDIA_SRCS = src/browser/media_player_ffmpeg.cpp
    MEDIA_FLAGS =
    MEDIA_LIBS = $(shell pkg-config --libs libavcodec libavformat libavutil libswscale libswresample) -ldl -lm
    MEDIA_CFLAGS = $(shell pkg-config --cflags libavcodec libavformat libavutil libswscale libswresample)
    GL_LIBS = -lGL
endif

TARGETS = stwp_server stwp_client stwp_browser

all: $(TARGETS)

$(OBJ_DIR)/%.o: $(IMGUI_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(GLFW_CFLAGS) $(IMGUI_INC) -c $< -o $@

$(OBJ_DIR)/%.o: $(IMGUI_DIR)/backends/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(GLFW_CFLAGS) $(IMGUI_INC) -c $< -o $@

$(OBJ_DIR)/yoga/%.o: $(YOGA_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(YOGA_INC) -c $< -o $@

$(OBJ_DIR)/lua/%.o: $(LUA_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -c $< -o $@

stwp_server: src/server/server.cpp src/common/tls.cpp $(COMMON_HDRS)
	$(CXX) $(CXXFLAGS) $(SSL_CFLAGS) src/server/server.cpp src/common/tls.cpp $(SSL_LIBS) -o stwp_server

stwp_client: src/client/client.cpp src/common/tls.cpp $(COMMON_HDRS)
	$(CXX) $(CXXFLAGS) $(SSL_CFLAGS) src/client/client.cpp src/common/tls.cpp $(SSL_LIBS) -o stwp_client

stwp_browser: src/browser/browser.cpp src/browser/globals.cpp src/browser/parser.cpp src/browser/fetcher.cpp src/browser/media_source.cpp src/browser/renderer.cpp src/browser/layout.cpp src/browser/script.cpp src/browser/script_fetch.cpp src/common/tls.cpp $(MEDIA_SRCS) $(IMGUI_OBJS) $(YOGA_OBJS) $(LUA_OBJS) $(COMMON_HDRS) $(BROWSER_HDRS)
	$(CXX) $(CXXFLAGS) $(MEDIA_FLAGS) $(MEDIA_CFLAGS) $(GLFW_CFLAGS) $(SSL_CFLAGS) $(IMGUI_INC) $(YOGA_INC) $(LUA_INC) \
		src/browser/browser.cpp src/browser/globals.cpp src/browser/parser.cpp src/browser/fetcher.cpp src/browser/media_source.cpp src/browser/renderer.cpp src/browser/layout.cpp src/browser/script.cpp src/browser/script_fetch.cpp src/common/tls.cpp $(MEDIA_SRCS) $(IMGUI_OBJS) $(YOGA_OBJS) $(LUA_OBJS) \
		$(GLFW_LIBS) $(GL_LIBS) $(MEDIA_LIBS) $(SSL_LIBS) -o stwp_browser

clean:
	rm -f stwp_server stwp_client stwp_browser
	rm -rf $(OBJ_DIR)

.PHONY: all clean
