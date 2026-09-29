#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SERVER_PATH = ROOT / "tools" / "lune_web.py"
APP_PATH = ROOT / "examples" / "web" / "app.lune"

spec = importlib.util.spec_from_file_location("lune_web", SERVER_PATH)
if spec is None or spec.loader is None:
    raise RuntimeError("could not load tools/lune_web.py")
lune_web = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lune_web)


def request(method: str, path: str, query: str = "", body: str = "", headers=None):
    return {
        "method": method,
        "target": path + (("?" + query) if query else ""),
        "path": path,
        "query": query,
        "headers": headers or {},
        "body": body,
        "remote_addr": "127.0.0.1",
    }


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_dev_server.py PATH_TO_LUNE", file=sys.stderr)
        return 2

    lune = str(Path(sys.argv[1]).resolve())
    app = str(APP_PATH.resolve())

    hello = lune_web.invoke_app(
        lune,
        app,
        request("GET", "/api/hello", "name=Alice+Blackrose"),
    )
    assert hello["status"] == 200
    assert hello["headers"]["content-type"] == "application/json; charset=utf-8"
    hello_body = json.loads(hello["body"])
    assert hello_body["message"] == "hello, Alice Blackrose"
    assert hello_body["method"] == "GET"

    echo = lune_web.invoke_app(
        lune,
        app,
        request(
            "POST",
            "/api/echo",
            body="hello from a request",
            headers={"content-type": "text/plain"},
        ),
    )
    assert echo["status"] == 200
    echo_body = json.loads(echo["body"])
    assert echo_body["body"] == "hello from a request"
    assert echo_body["content_type"] == "text/plain"

    missing = lune_web.invoke_app(lune, app, request("GET", "/missing"))
    assert missing["status"] == 404
    assert missing["body"] == "not found\n"

    print("web development server tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
