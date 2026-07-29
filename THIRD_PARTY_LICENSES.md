# Third-Party Licenses

StarWeb's own code is MIT-licensed (see `LICENSE`). It depends on the
following third-party software, either vendored directly in `src/thirdparty/`
or linked as a system/vcpkg dependency.

## FFmpeg (Windows / Linux media backend)

- **License**: GNU Lesser General Public License, version 2.1 or later (LGPL-2.1+).
- **How it's used**: dynamically linked (`.dll` on Windows, `.so` on Linux) via
  `avcodec`, `avdevice`, `avfilter`, `avformat`, `swresample`, `swscale`: FFmpeg's
  default feature set. StarWeb does not enable the `gpl` or `nonfree` vcpkg/build
  features (no libx264, libx265, libvpx, libmp3lame, fdk-aac, etc.), so the build
  stays under LGPL rather than GPL, and contains no unredistributable components.
- **Unmodified**: StarWeb uses stock upstream FFmpeg; no patches are applied.
- **Source**: https://ffmpeg.org; obtain matching source via `vcpkg` (Windows) or
  your distro's package source (Linux), or from the FFmpeg project directly.
- **Compliance notes**: dynamic linking means the LGPL's relinking requirement is
  satisfied by construction (swap the `.dll`/`.so` for a compatible build and the
  binaries still work). This file, plus `vcpkg`'s bundled
  `share/ffmpeg/copyright` (LGPL-2.1 full text), serves as the required license
  notice for redistributed binaries.

## OpenSSL

- **License**: Apache License 2.0.
- **How it's used**: system/vcpkg dependency (not vendored), linked into all three
  binaries for the TLS 1.3 transport under the `star://` scheme.
- **Unmodified**: StarWeb uses stock upstream OpenSSL 3; no patches are applied.
- **Source**: https://www.openssl.org

## GLFW

- **License**: zlib/libpng license.
- **How it's used**: system/vcpkg dependency (not vendored) for windowing and
  OpenGL context creation.
- **Source**: https://www.glfw.org

## Dear ImGui

- **License**: MIT.
- **How it's used**: vendored in `src/thirdparty/imgui/`, used unmodified for the
  browser's UI.
- **License text**: `src/thirdparty/imgui/LICENSE.txt`
- **Source**: https://github.com/ocornut/imgui

## miniaudio

- **License**: public domain (Unlicense) or MIT-0, dual-licensed; user's choice.
- **How it's used**: vendored single-header library (`src/thirdparty/miniaudio.h`)
  for cross-platform audio output on the Windows/Linux FFmpeg media backend.
- **Source**: https://miniaud.io

## stb_image

- **License**: public domain, with an MIT-equivalent license offered as an
  alternative in the file for jurisdictions that don't recognize public domain
  dedications.
- **How it's used**: vendored single-header library (`src/thirdparty/stb_image.h`)
  for decoding images (PNG/JPEG/etc.) embedded in fetched pages.
- **Source**: https://github.com/nothings/stb

## Lua

- **License**: MIT.
- **Version**: 5.4.7 (unmodified upstream sources).
- **How it's used**: vendored in `src/thirdparty/lua/`, compiled as a generic
  ANSI C static library (no `LUA_USE_*` platform macros, so `dlopen`/`loadlib`
  are disabled). Provides the browser's page-scripting engine; page scripts run
  in a deny-by-default sandbox (see `src/browser/script.{hpp,cpp}`).
- **License text**: the MIT notice is in the copyright block at the end of
  `src/thirdparty/lua/lua.h`.
- **Source**: https://www.lua.org

## Inter

- **License**: SIL Open Font License 1.1.
- **Version**: 4.1 (static `Inter-SemiBold.ttf` from the `extras/ttf` set; the
  `.otf` builds use CFF outlines, which the ImGui rasterizer cannot read).
- **How it's used**: bundled in `fonts/`, loaded at startup and registered as a
  named face pages can select with `font-family: Inter SemiBold`.
- **License text**: `fonts/Inter-LICENSE.txt`.
- **Source**: https://github.com/rsms/inter
