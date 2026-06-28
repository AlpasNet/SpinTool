#!/usr/bin/env python3
from __future__ import annotations

import argparse
import http.server
import mimetypes
import os
from pathlib import Path

mimetypes.add_type("application/wasm", ".wasm")
mimetypes.add_type("application/javascript", ".js")


class SpinToolHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-store")
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()


def main() -> None:
    parser = argparse.ArgumentParser(description="Serve a SpinTool Web build locally.")
    parser.add_argument("directory", nargs="?", default="build-web")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    directory = Path(args.directory).resolve()
    if not (directory / "index.html").is_file():
        raise SystemExit(f"No index.html found in {directory}")

    os.chdir(directory)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), SpinToolHandler)
    print(f"SpinTool Web: http://127.0.0.1:{args.port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
