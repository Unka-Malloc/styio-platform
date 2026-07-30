#!/usr/bin/env python3
from __future__ import annotations

import argparse
import shutil
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Final
from urllib.parse import unquote, urlsplit


REQUEST_TIMEOUT_SECONDS: Final = 10.0
CONTENT_TYPES: Final = {
    ".json": "application/json; charset=utf-8",
    ".jsonl": "application/x-ndjson; charset=utf-8",
    ".tar": "application/x-tar",
}


def is_relative_to(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
    except ValueError:
        return False
    return True


def content_type_for(path: Path) -> str:
    if path.name.endswith(".pafio.src.tar"):
        return "application/x-tar"
    return CONTENT_TYPES.get(path.suffix, "application/octet-stream")


def resolve_registry_file(registry_root: Path, request_target: str) -> Path:
    parsed = urlsplit(request_target)
    decoded = unquote(parsed.path)
    if "\x00" in decoded:
        raise ValueError("request path contains a NUL byte")
    if "\\" in decoded:
        raise ValueError("registry object paths must use POSIX separators")
    relative_text = decoded.lstrip("/")
    if not relative_text:
        raise IsADirectoryError("directory listing is disabled")
    parts = relative_text.split("/")
    if any(part in {"", ".", ".."} for part in parts):
        raise ValueError("registry object path is not canonical")
    root = registry_root.resolve()
    candidate = (root / relative_text).resolve()
    if not is_relative_to(candidate, root):
        raise PermissionError("registry object path escapes the registry root")
    return candidate


class RegistryStaticReadHandler(BaseHTTPRequestHandler):
    registry_root: Path

    def setup(self) -> None:
        super().setup()
        self.connection.settimeout(REQUEST_TIMEOUT_SECONDS)

    def log_message(self, format: str, *args: object) -> None:
        return

    def do_HEAD(self) -> None:
        self._serve(send_body=False)

    def do_GET(self) -> None:
        self._serve(send_body=True)

    def do_POST(self) -> None:
        self.send_error(HTTPStatus.METHOD_NOT_ALLOWED, "registry read plane is read-only")

    def do_PUT(self) -> None:
        self.send_error(HTTPStatus.METHOD_NOT_ALLOWED, "registry read plane is read-only")

    def do_DELETE(self) -> None:
        self.send_error(HTTPStatus.METHOD_NOT_ALLOWED, "registry read plane is read-only")

    def _serve(self, *, send_body: bool) -> None:
        try:
            file_path = resolve_registry_file(self.registry_root, self.path)
        except IsADirectoryError as err:
            self.send_error(HTTPStatus.NOT_FOUND, str(err))
            return
        except PermissionError as err:
            self.send_error(HTTPStatus.FORBIDDEN, str(err))
            return
        except ValueError as err:
            self.send_error(HTTPStatus.BAD_REQUEST, str(err))
            return

        if file_path.is_dir():
            self.send_error(HTTPStatus.NOT_FOUND, "directory listing is disabled")
            return
        if not file_path.is_file():
            self.send_error(HTTPStatus.NOT_FOUND, "registry object was not found")
            return

        size = file_path.stat().st_size
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type_for(file_path))
        self.send_header("Content-Length", str(size))
        self.send_header("Cache-Control", cache_control_for(file_path))
        self.end_headers()
        if send_body:
            with file_path.open("rb") as source:
                shutil.copyfileobj(source, self.wfile)


def cache_control_for(path: Path) -> str:
    relative_parts = set(path.parts)
    if "artifacts" in relative_parts or "leaves" in relative_parts:
        return "public, max-age=31536000, immutable"
    return "public, max-age=60"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Serve a pafio registry v2 root as a read-only HTTP static read plane.")
    parser.add_argument("--root", required=True, help="Local directory containing the registry v2 static root.")
    parser.add_argument("--bind", default="127.0.0.1", help="Address to bind for registry reads.")
    parser.add_argument("--port", type=int, required=True, help="Port to bind for registry reads.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = Path(args.root).resolve()
    if not root.exists() or not root.is_dir():
        raise SystemExit(f"registry root is not a directory: {root}")
    RegistryStaticReadHandler.registry_root = root
    server = ThreadingHTTPServer((args.bind, args.port), RegistryStaticReadHandler)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
