# Building StarWeb

The project builds on **macOS, Linux, and Windows**. There are three binaries:
`stwp_server`, `stwp_client`, and `stwp_browser`.

The media player in the browser uses a platform-specific backend, selected
automatically by the build system:

| Platform | Media backend | Source file |
|----------|---------------|-------------|
| macOS    | AVFoundation (native, hardware-accelerated) | `src/browser/media_player_mac.mm` |
| Linux    | FFmpeg decode + miniaudio output | `src/browser/media_player_ffmpeg.cpp` |
| Windows  | FFmpeg decode + miniaudio output | `src/browser/media_player_ffmpeg.cpp` |

`miniaudio` is vendored (`src/thirdparty/miniaudio.h`); FFmpeg is a system
dependency on Linux/Windows only. **OpenSSL 3** is a system dependency on every
platform — it provides the TLS 1.3 transport for the `star://` scheme. See
`THIRD_PARTY_LICENSES.md` for the license terms of FFmpeg, OpenSSL, and every
other dependency.

To serve `star://` you also need a certificate. Run `tools/make_certs.sh` once
to generate a local StarWeb root CA and a `localhost` server cert under `certs/`.

---

## macOS

**Prerequisites**: Xcode command-line tools (a compiler), GLFW, and OpenSSL 3.

```sh
brew install glfw openssl@3
make            # or: cmake -S . -B build && cmake --build build
```

`openssl@3` is keg-only on Homebrew; the Makefile and CMake both locate it
automatically at `/usr/local/opt/openssl@3` (Intel) or `/opt/homebrew/opt/openssl@3`
(Apple Silicon), so no `PKG_CONFIG_PATH` export is needed.

Binaries land in the project root (`stwp_server`, `stwp_client`, `stwp_browser`).

---

## Linux

**Prerequisites**: a compiler, GLFW, OpenGL, OpenSSL 3, and the FFmpeg
development libraries.

```sh
# Debian / Ubuntu
sudo apt install build-essential pkg-config libglfw3-dev libgl1-mesa-dev libssl-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev

make            # or: cmake -S . -B build && cmake --build build
```

Binaries land in the project root (`stwp_server`, `stwp_client`, `stwp_browser`).
Audio output uses ALSA or PulseAudio, discovered at runtime by miniaudio — no
extra build-time audio dependency is required.

---

## Windows

**Prerequisites**:

- **MSVC**: [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022)
  (or full Visual Studio) with the "Desktop development with C++" workload. Without
  this, `cmake` has no compiler to find.
- **GLFW, FFmpeg, and OpenSSL** via [vcpkg](https://vcpkg.io) (the easiest way to get them on Windows).

```powershell
vcpkg install glfw3:x64-windows ffmpeg:x64-windows openssl:x64-windows
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

Replace `C:/path/to/vcpkg` with your actual vcpkg checkout (e.g. `$env:VCPKG_ROOT`
if that's set, or wherever you cloned it — it's a placeholder, not a literal path).
Binaries land in `build\Release\`.

**Troubleshooting**:

- *"CMAKE_C_COMPILER not set" / generator errors*: without an explicit generator,
  CMake can default to `NMake Makefiles` in a plain PowerShell window, which then
  fails because NMake needs a Developer Command Prompt environment (`vcvarsall.bat`)
  that a regular shell doesn't have. The `-G "Visual Studio 17 2022" -A x64` flags
  above sidestep this — that generator locates MSVC itself.
- *`LNK1104: cannot open ... .exe`*: one of the built binaries is still running
  (Windows locks the file while it's open) — close it and rebuild.
- Winsock (`ws2_32`) is linked automatically; no extra step needed.

### Optional: also building the Linux binaries, via WSL

You can build and run the Linux binaries from the same Windows machine using WSL2,
without dual-booting or a VM.

**One-time setup**:

```powershell
wsl --install -d Ubuntu
wsl -d Ubuntu -u root -- bash -c "apt-get update && apt-get install -y \
    build-essential pkg-config libglfw3-dev libgl1-mesa-dev libssl-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
    cmake libpulse0 pulseaudio-utils alsa-utils"
```

**Build** (run from Windows; `/mnt/e/...` maps to `E:\...` — adjust the drive letter):

```powershell
wsl -d Ubuntu -u root -- bash -c "cd /mnt/e/Github_projects/StarWeb && \
    cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build-linux --parallel"
```

**Run** (see the "Running the binaries" note below for why `cd` to the project root matters):

```powershell
wsl -d Ubuntu -u root -- bash -c "cd /mnt/e/Github_projects/StarWeb && ./build-linux/stwp_server"
wsl -d Ubuntu -u root -- bash -c "cd /mnt/e/Github_projects/StarWeb && ./build-linux/stwp_browser"
```

On Windows 11, WSLg shows `stwp_browser`'s GUI as a normal desktop window
automatically — no X server setup needed. `libpulse0`/`alsa-utils` aren't in a
minimal WSL image by default; without them miniaudio has no audio backend to
load, and video/audio play back silently with no error.

---

## Running the binaries

**Generate the TLS certificates first** — the browser opens `star://` by default,
so without them it has nothing to load. Then start the server and the browser (or
client), each in its own terminal:

```sh
./tools/make_certs.sh.  # once: local root CA + localhost cert, written to certs/
./stwp_server.          # terminal 1: moon:// on 8090, star:// on 8490
./stwp_browser.         # terminal 2, or ./stwp_client star://localhost/<path>
```

`certs/` is git-ignored, so a fresh clone needs this step once. Skipping it leaves
the server serving plaintext `moon://` on 8090 only (it logs the reason and
disables `star://`), and the browser's default page fails to load until you either
run the script or navigate to a `moon://` URL. See `PROTOCOL.md` for the scheme
and TLS details.

**Run both from the project root**, not from inside `build/`. `stwp_server`
resolves content paths relative to its current working directory (`www/` +
the requested path), so if you `cd` into the build directory first, it'll
report `File not found` for everything. This applies on every platform.

---

## Notes

- **Networking** is fully portable via `src/common/net.hpp`, a thin socket
  compatibility layer (POSIX sockets on macOS/Linux, Winsock2 on Windows). Use
  `net::socket_t` / `net::kInvalidSocket` rather than raw `int` / `-1`.
- **Transports** go through `src/common/conn.hpp`: `PlainConn` for `moon://`,
  `TlsConn` (`src/common/tls.hpp`) for `star://`. STWP message code reads and
  writes a `Conn` and does not care which is underneath.
- To compile-check the FFmpeg backend on a Mac (which otherwise builds the
  AVFoundation path), define `STWP_FORCE_FFMPEG` and provide the FFmpeg headers.
