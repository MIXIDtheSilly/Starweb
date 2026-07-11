CXX = clang++
CXXFLAGS = -std=c++17 -Wall -Wextra -O3 -pthread

IMGUI_DIR = src/thirdparty/imgui
IMGUI_INC = -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends

# Detect GLFW using pkg-config
GLFW_CFLAGS = $(shell pkg-config --cflags glfw3 2>/dev/null || echo "")
GLFW_LIBS = $(shell pkg-config --libs glfw3 2>/dev/null || echo "-lglfw")

# Object build directory
OBJ_DIR = obj

# ImGui object files
IMGUI_OBJS = $(OBJ_DIR)/imgui.o \
             $(OBJ_DIR)/imgui_draw.o \
             $(OBJ_DIR)/imgui_widgets.o \
             $(OBJ_DIR)/imgui_tables.o \
             $(OBJ_DIR)/imgui_impl_glfw.o \
             $(OBJ_DIR)/imgui_impl_opengl3.o

TARGETS = stwp_server stwp_client stwp_browser

all: $(TARGETS)

# Rules to compile ImGui source files to object files
$(OBJ_DIR)/%.o: $(IMGUI_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(GLFW_CFLAGS) $(IMGUI_INC) -c $< -o $@

$(OBJ_DIR)/%.o: $(IMGUI_DIR)/backends/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(GLFW_CFLAGS) $(IMGUI_INC) -c $< -o $@

stwp_server: src/server/server.cpp src/common/stwp_msg.hpp
	$(CXX) $(CXXFLAGS) src/server/server.cpp -o stwp_server

stwp_client: src/client/client.cpp src/common/url_parser.hpp src/common/stwp_msg.hpp
	$(CXX) $(CXXFLAGS) src/client/client.cpp -o stwp_client

stwp_browser: src/browser/browser.cpp src/browser/globals.cpp src/browser/parser.cpp src/browser/fetcher.cpp src/browser/renderer.cpp $(IMGUI_OBJS) src/common/url_parser.hpp src/common/stwp_msg.hpp
	$(CXX) $(CXXFLAGS) $(GLFW_CFLAGS) $(IMGUI_INC) \
		src/browser/browser.cpp src/browser/globals.cpp src/browser/parser.cpp src/browser/fetcher.cpp src/browser/renderer.cpp $(IMGUI_OBJS) \
		$(GLFW_LIBS) -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo -o stwp_browser

clean:
	rm -f stwp_server stwp_client stwp_browser
	rm -rf $(OBJ_DIR)

.PHONY: all clean
