#!/usr/bin/env python3
"""Small development HTTP server for Lune applications.

Each request is serialized as JSON and passed as the sole argument to a Lune
application. The application must print one JSON response object to stdout.
This adapter is intentionally development-oriented: it starts one Lune process
per request and is not intended to be a production server.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit


class AppError(RuntimeError):
    pass


def _validate_response(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise AppError("application response must be a JSON object")

    status = value.get("status")
    headers = value.get("headers", {})
    body = value.get("body", "")

    if not isinstance(status, int) or not 100 <= status <= 599:
        raise AppError("application response status must be an integer from 100 to 599")
    if not isinstance(headers, dict):
        raise AppError("application response headers must be an object")
    if not isinstance(body, str):
        raise AppError("application response body must be a string")

    clean_headers: dict[str, str] = {}
    for name, header_value in headers.items():
        if not isinstance(name, str) or not isinstance(header_value, str):
            raise AppError("application response header names and values must be strings")
        if "\r" in name or "\n" in name or "\r" in header_value or "\n" in header_value:
            raise AppError("application response headers may not contain CR or LF")
        clean_headers[name] = header_value

    return {"status": status, "headers": clean_headers, "body": body}


def invoke_app(
    lune: str,
    app: str,
    request: dict[str, Any],
    *,
    timeout: float = 10.0,
) -> dict[str, Any]:
    payload = json.dumps(request, ensure_ascii=False, separators=(",", ":"))

    try:
        completed = subprocess.run(
            [lune, app, payload],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as exc:
        raise AppError(f"application timed out after {timeout:g} seconds") from exc
    except OSError as exc:
        raise AppError(f"failed to start Lune application: {exc}") from exc

    if completed.returncode != 0:
        detail = completed.stderr.strip() or f"exit status {completed.returncode}"
        raise AppError(f"application failed: {detail}")

    output = completed.stdout.strip()
    if not output:
        raise AppError("application produced no JSON response on stdout")

    try:
        value = json.loads(output)
    except json.JSONDecodeError as exc:
        raise AppError(
            "application stdout was not one JSON response; use eprint() for debug logging"
        ) from exc

    return _validate_response(value)


class LuneRequestHandler(BaseHTTPRequestHandler):
    server_version = "lune-web/0.1"

    def _send_text_error(self, status: int, message: str) -> None:
        body = (message.rstrip() + "\n").encode("utf-8")
        self.send_response(status)
        self.send_header("content-type", "text/plain; charset=utf-8")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _handle(self) -> None:
        server = self.server
        assert isinstance(server, LuneHTTPServer)

        raw_length = self.headers.get("content-length")
        content_length = 0
        if raw_length:
            try:
                content_length = int(raw_length)
            except ValueError:
                self._send_text_error(400, "invalid Content-Length")
                return

        if content_length < 0:
            self._send_text_error(400, "invalid Content-Length")
            return
        if content_length > server.max_body:
            self._send_text_error(413, "request body too large")
            return

        raw_body = self.rfile.read(content_length) if content_length else b""
        try:
            body = raw_body.decode("utf-8")
        except UnicodeDecodeError:
            self._send_text_error(400, "development server accepts UTF-8 request bodies only")
            return

        target = urlsplit(self.path)
        headers = {name.lower(): value for name, value in self.headers.items()}
        request = {
            "method": self.command,
            "target": self.path,
            "path": target.path or "/",
            "query": target.query,
            "headers": headers,
            "body": body,
            "remote_addr": self.client_address[0],
        }

        try:
            response = invoke_app(
                server.lune,
                server.app,
                request,
                timeout=server.app_timeout,
            )
        except AppError as exc:
            print(f"lune-web: {exc}", file=sys.stderr)
            self._send_text_error(500, "Lune application error")
            return

        encoded_body = response["body"].encode("utf-8")
        self.send_response(response["status"])

        has_content_type = False
        for name, value in response["headers"].items():
            lowered = name.lower()
            if lowered == "content-length":
                continue
            if lowered == "content-type":
                has_content_type = True
            self.send_header(name, value)

        if not has_content_type:
            self.send_header("content-type", "text/plain; charset=utf-8")
        self.send_header("content-length", str(len(encoded_body)))
        self.end_headers()

        if self.command != "HEAD":
            self.wfile.write(encoded_body)

    do_GET = _handle
    do_HEAD = _handle
    do_POST = _handle
    do_PUT = _handle
    do_PATCH = _handle
    do_DELETE = _handle
    do_OPTIONS = _handle


class LuneHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(
        self,
        address: tuple[str, int],
        lune: str,
        app: str,
        *,
        app_timeout: float,
        max_body: int,
    ) -> None:
        super().__init__(address, LuneRequestHandler)
        self.lune = lune
        self.app = app
        self.app_timeout = app_timeout
        self.max_body = max_body


def _default_lune() -> str:
    configured = os.environ.get("LUNE")
    if configured:
        return configured

    installed = shutil.which("lune")
    if installed:
        return installed

    return str(Path("bootstrap") / "build" / "lune")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="development HTTP server for Lune apps")
    parser.add_argument("app", help="Lune application file")
    parser.add_argument("--lune", default=_default_lune(), help="path to the Lune executable")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--timeout", type=float, default=10.0, help="per-request app timeout in seconds")
    parser.add_argument("--max-body", type=int, default=1024 * 1024, help="maximum request body size in bytes")
    args = parser.parse_args(argv)

    app = str(Path(args.app).resolve())
    lune = str(Path(args.lune).resolve()) if os.sep in args.lune else args.lune

    if not Path(app).is_file():
        parser.error(f"application file does not exist: {app}")
    if args.port < 0 or args.port > 65535:
        parser.error("--port must be from 0 to 65535")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.max_body < 0:
        parser.error("--max-body must not be negative")

    server = LuneHTTPServer(
        (args.host, args.port),
        lune,
        app,
        app_timeout=args.timeout,
        max_body=args.max_body,
    )

    print(
        f"lune-web: http://{args.host}:{server.server_port} -> {app}",
        file=sys.stderr,
    )

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
