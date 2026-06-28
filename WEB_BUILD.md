# SpinTool Web / WebAssembly build

SpinTool keeps its C++ ROM engine and Dear ImGui interface. The web target
compiles the same project to WebAssembly with Emscripten and SDL3.

## Requirements

- Emscripten SDK, latest stable release
- CMake 3.25 or newer
- Git or internet access during the first CMake configuration
- Python 3 for the local test server

SDL3 3.4.10 is downloaded by CMake and verified with its SHA-256 checksum.
SDL3_image is not required by the browser target. PNG loading and saving use
Emscripten's built-in libpng/zlib ports through a small SDL3-compatible layer.
Indexed PNG files remain indexed so tile colour IDs are preserved.

## Build

```bash
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh

cd /path/to/SpinTool
./web/build-web.sh
```

The output is written to `build-web/`:

- `index.html`
- `index.js`
- `index.wasm`

## Run locally

WebAssembly must be served through HTTP; opening `index.html` directly is not
supported.

```bash
python3 web/serve.py build-web
```

Then open `http://127.0.0.1:8080`.

## Browser workflow

- **Open ROM** opens the browser file picker and imports a `.bin` or `.md` ROM.
- The immutable reference is stored in `/spintool/roms`.
- The edited copy is stored in `/spintool/rom_export`.
- Files and preferences are persisted in the browser with IndexedDB.
- **Download modified ROM** downloads the current edited ROM.
- PNG imports use the browser's native file picker.
- PNG exports are downloaded immediately.
- Processing stays in the browser; SpinTool does not upload the ROM or images
  to a server.

Browser storage belongs to the exact website origin. Clearing site data also
clears SpinTool's persisted ROMs, projects and preferences, so download the
modified ROM before clearing browser data.

## GitHub Actions

The workflow `.github/workflows/build-web.yml` builds the WebAssembly version
on pushes, pull requests and manual runs. Its `SpinTool-Web` artifact contains
the three files needed for static hosting.

## Hosting

Upload all files from `build-web/` to the same directory on any static HTTP
host. The server must serve `.wasm` as `application/wasm`.

The supplied local server also sends COOP/COEP headers. The current SpinTool
web build does not require pthreads, but those headers keep the package ready
for optional WebAssembly threads later.

## Current web-specific behaviour

- The main loop is driven by the browser rather than a blocking desktop loop.
- The full sprite scan runs synchronously in this first web port. A very large
  scan can temporarily pause the interface until the scan completes.
- System font discovery is unavailable in the browser. Fonts placed in the
  browser filesystem under `/spintool/fonts` remain supported by the existing
  font loader.
