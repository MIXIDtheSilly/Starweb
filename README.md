# <p style="font-size:32px">StarWeb</p>
## <p style="font-size:20px">StarWeb is a custom web ecosystem built from scratch in C++.</p>

### <p style="font-size:20px">StarWeb consists of 2 (important) elements: 
- STWP (StarWeb Transfer Protocol) — A protocol similar in structure to HTTP but built entirely from scratch. URL format `moon://host:port/path` for fetching static files.

- Starmap — A custom browser for use with STWP, it supports rendering of pages over local or hosted StarWeb servers, avaiable with full tab UI, HTML/CSS rendering, image loading and media playback.
</p>

# Overview
### The whole implementation consists of three binaries
- stwp_server — serves files from a local `www/` directory over STWP.
- stwp_client — minimal command-line client for sending STWP requests.
- stwp_browser — full ImGui-based browser with tabs, navigation and rendering

# Building

### Dependencies
- clang++ with C++17 support
- GLFW 3
- OpenGL
- ImGui (place at /src/thirdparty/imgui/)

### macOS
```sh
brew install glfw
git clone https://github.com/ocornut/imgui src/thirdparty/imgui
make
```

# Running

Start the server first, then launch the browser:
```sh
./stwp_server      # serves www/ on port 8090
./stwp_browser     # opens to moon://localhost/index.html
```
The server accepts PORT arguments:
```sh
./stwp_server {PORT}
```
Put your HTML, CSS and assets in the `www/` directory. The server maps requests directly to files in that folder.

# Protocol
STWP support only GET requests for now. The browser uses the `moon://` scheme. Message framing and header format are defined in `src/common/stwp_msg.hpp`.
### Supported URL schemes in the browser:
| Scheme  | Default Port | Purpose                     |
|---------|--------------|-----------------------------|
| moon:// | 8090         | Standard STWP server        |
| star:// | 8490         | Reserved / alternate server |

# Repository Layout
```text
.
├──src/
|    ├──browser/    Browser frontend (ImGui, renderer, parser, fetcher, media)
|    ├──server/     STWP file server
|    ├──client/     CLI STWP client
|    ├──common/     Shared protocol and URL parsing headers
├──www/            Web root served by stwp_server
├──makefile
└──README.md
```
