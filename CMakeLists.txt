cmake_minimum_required(VERSION 3.16)
project(starweb LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(SRC ${CMAKE_SOURCE_DIR}/src)
set(IMGUI_DIR ${SRC}/thirdparty/imgui)

find_package(Threads REQUIRED)

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)

set(YOGA_DIR ${SRC}/thirdparty/yoga)
file(GLOB_RECURSE YOGA_SRCS ${YOGA_DIR}/yoga/*.cpp)
if(NOT YOGA_SRCS)
    message(FATAL_ERROR
        "No Yoga sources found under ${YOGA_DIR}/yoga.\n"
        "The submodule is probably not checked out. Run:\n"
        "  git submodule update --init --recursive")
endif()
add_library(yoga STATIC ${YOGA_SRCS})
target_include_directories(yoga PUBLIC ${YOGA_DIR})
target_compile_features(yoga PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(yoga PUBLIC /FI${SRC}/thirdparty/yoga_compat.hpp)
else()
    target_compile_options(yoga PUBLIC -include ${SRC}/thirdparty/yoga_compat.hpp)
endif()

set(LUA_DIR ${SRC}/thirdparty/lua)
file(GLOB LUA_SRCS ${LUA_DIR}/*.c)
add_library(lua STATIC ${LUA_SRCS})
target_include_directories(lua PUBLIC ${LUA_DIR})

set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)

# brew keeps openssl@3 keg-only; give CMake a hint if the default search misses.
if(APPLE AND NOT OPENSSL_ROOT_DIR)
    foreach(prefix /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
        if(EXISTS ${prefix})
            set(OPENSSL_ROOT_DIR ${prefix})
            break()
        endif()
    endforeach()
endif()
find_package(OpenSSL 3 REQUIRED)

find_package(glfw3 QUIET)
if(glfw3_FOUND)
    set(GLFW_TARGET glfw)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLFW REQUIRED glfw3)
    add_library(glfw_pc INTERFACE)
    target_include_directories(glfw_pc INTERFACE ${GLFW_INCLUDE_DIRS})
    target_link_libraries(glfw_pc INTERFACE ${GLFW_LIBRARIES})
    target_link_directories(glfw_pc INTERFACE ${GLFW_LIBRARY_DIRS})
    set(GLFW_TARGET glfw_pc)
endif()
target_link_libraries(imgui PUBLIC ${GLFW_TARGET} OpenGL::GL)

set(NET_LIBS "")
if(WIN32)
    set(NET_LIBS ws2_32)
endif()

add_executable(stwp_server ${SRC}/server/server.cpp ${SRC}/common/tls.cpp)
target_link_libraries(stwp_server PRIVATE Threads::Threads OpenSSL::SSL OpenSSL::Crypto ${NET_LIBS})

add_executable(stwp_client ${SRC}/client/client.cpp ${SRC}/common/tls.cpp)
target_link_libraries(stwp_client PRIVATE Threads::Threads OpenSSL::SSL OpenSSL::Crypto ${NET_LIBS})

file(GLOB PROXY_SRCS ${SRC}/proxy/*.cpp)
add_executable(stwp_proxy ${PROXY_SRCS} ${SRC}/common/tls.cpp)
target_link_libraries(stwp_proxy PRIVATE Threads::Threads OpenSSL::SSL OpenSSL::Crypto ${NET_LIBS})

set(BROWSER_SRCS
    ${SRC}/browser/browser.cpp
    ${SRC}/browser/globals.cpp
    ${SRC}/browser/parser.cpp
    ${SRC}/browser/fetcher.cpp
    ${SRC}/browser/conn_pool.cpp
    ${SRC}/browser/media_source.cpp
    ${SRC}/browser/renderer.cpp
    ${SRC}/browser/icons.cpp
    ${SRC}/browser/layout.cpp
    ${SRC}/browser/zoom.cpp
    ${SRC}/browser/devtools.cpp
    ${SRC}/browser/script.cpp
    ${SRC}/browser/script_fetch.cpp
    ${SRC}/browser/storage.cpp
    ${SRC}/browser/cookies.cpp
    ${SRC}/browser/history.cpp
    ${SRC}/browser/theme.cpp
    ${SRC}/common/tls.cpp
)

add_executable(stwp_browser ${BROWSER_SRCS})
target_link_libraries(stwp_browser PRIVATE imgui yoga lua Threads::Threads OpenSSL::SSL OpenSSL::Crypto ${NET_LIBS})

# Embed the public StarWeb root CA in the browser.
set(STARWEB_ROOT_CA ${CMAKE_SOURCE_DIR}/certs/starweb_root.pem)
if(NOT EXISTS ${STARWEB_ROOT_CA})
    message(FATAL_ERROR "${STARWEB_ROOT_CA} not found: run tools/make_certs.sh")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${STARWEB_ROOT_CA})
file(READ ${STARWEB_ROOT_CA} STARWEB_ROOT_PEM)
configure_file(${SRC}/browser/embedded_ca.hpp.in ${CMAKE_BINARY_DIR}/generated/embedded_ca.hpp @ONLY)
target_include_directories(stwp_browser PRIVATE ${CMAKE_BINARY_DIR}/generated)

if(WIN32)
    set(STARMAP_ICO ${SRC}/browser/img/starmap.ico)
    set(STARMAP_MANIFEST_RESOURCE "")
    if(MINGW)
        # Linked before MinGW's default-manifest.o, so ld keeps this manifest.
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(STARMAP_ARCH amd64)
        else()
            set(STARMAP_ARCH x86)
        endif()
        set(STARMAP_MANIFEST ${CMAKE_BINARY_DIR}/CMakeFiles/stwp_browser.manifest)
        configure_file(${SRC}/browser/stwp_browser.manifest.in ${STARMAP_MANIFEST} @ONLY)
        set(STARMAP_MANIFEST_RESOURCE "1 24 \"${STARMAP_MANIFEST}\"")
    endif()
    set(STARMAP_RC ${CMAKE_BINARY_DIR}/CMakeFiles/starmap.rc)
    configure_file(${SRC}/browser/starmap.rc.in ${STARMAP_RC} @ONLY)
    target_sources(stwp_browser PRIVATE ${STARMAP_RC})
    set_property(SOURCE ${STARMAP_RC} APPEND PROPERTY OBJECT_DEPENDS ${STARMAP_ICO} ${STARMAP_MANIFEST})

    # No console window; run with --console to get one.
    set_target_properties(stwp_browser PROPERTIES WIN32_EXECUTABLE ON)
    if(MSVC)
        target_link_options(stwp_browser PRIVATE /ENTRY:mainCRTStartup)
    endif()
endif()

if(APPLE)
    target_sources(stwp_browser PRIVATE ${SRC}/browser/media_player_mac.mm)
    set_source_files_properties(${SRC}/browser/media_player_mac.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(stwp_browser PRIVATE
        "-framework AVFoundation" "-framework CoreMedia"
        "-framework AudioToolbox" "-framework QuartzCore"
        "-framework Cocoa" "-framework IOKit" "-framework CoreVideo")
else()
    target_sources(stwp_browser PRIVATE ${SRC}/browser/media_player_ffmpeg.cpp)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec libavformat libavutil libswscale libswresample)
    target_link_libraries(stwp_browser PRIVATE PkgConfig::FFMPEG)

    if(UNIX)
        target_link_libraries(stwp_browser PRIVATE ${CMAKE_DL_LIBS} m)
    endif()
endif()

# One folder per program. $<0:> stops Visual Studio adding Release/ or Debug/.
foreach(app browser server client proxy)
    set_target_properties(stwp_${app} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${app}$<0:>)
endforeach()

file(GLOB BROWSER_FONTS ${CMAKE_SOURCE_DIR}/fonts/*)
add_custom_command(TARGET stwp_browser POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:stwp_browser>/fonts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BROWSER_FONTS} $<TARGET_FILE_DIR:stwp_browser>/fonts
    VERBATIM)

add_custom_command(TARGET stwp_proxy POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:stwp_proxy>/conf
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/conf/stwp_proxy.conf $<TARGET_FILE_DIR:stwp_proxy>/conf
    VERBATIM)

# vcpkg already copies DLLs next to MSVC builds; MinGW needs it done by hand.
if(MINGW)
    get_filename_component(MINGW_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
    foreach(exe stwp_server stwp_client stwp_proxy stwp_browser)
        set(dll_subdir_args "")
        if(exe STREQUAL stwp_browser)
            set(dll_subdir_args -DDLL_SUBDIR=lib -DARCH=${STARMAP_ARCH})
        endif()
        add_custom_command(TARGET ${exe} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DEXE=$<TARGET_FILE:${exe}>
                -DSEARCH_DIRS=${MINGW_BIN_DIR}
                -DOBJDUMP=${CMAKE_OBJDUMP}
                ${dll_subdir_args}
                -P ${CMAKE_SOURCE_DIR}/tools/copy_runtime_dlls.cmake
            VERBATIM)
    endforeach()
endif()
