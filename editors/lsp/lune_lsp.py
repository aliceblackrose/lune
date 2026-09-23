#!/usr/bin/env python3
"""Minimal dependency-free Language Server Protocol adapter for Lune 0.1."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Any

_DIAGNOSTIC_HEADER = re.compile(r"^.*:(\d+):(\d+): error: (.+)$")


def resolve_lune() -> str | None:
    configured = os.environ.get("LUNE_BIN")
    if configured:
        return configured

    installed = shutil.which("lune")
    if installed:
        return installed

    candidate = (
        Path(__file__).resolve().parents[2]
        / "bootstrap"
        / "build"
        / "lune"
    )
    if candidate.is_file():
        return str(candidate)

    return None


def utf16_units(text: str) -> int:
    return len(text.encode("utf-16-le")) // 2


def byte_column_to_utf16(line: str, one_based_column: int) -> int:
    byte_offset = max(0, one_based_column - 1)
    prefix = line.encode("utf-8")[:byte_offset].decode("utf-8", "ignore")
    return utf16_units(prefix)


def byte_span_to_utf16(line: str, one_based_column: int, byte_length: int) -> int:
    encoded = line.encode("utf-8")
    start = max(0, one_based_column - 1)
    end = min(len(encoded), start + max(1, byte_length))
    span = encoded[start:end].decode("utf-8", "ignore")
    return max(1, utf16_units(span))


def parse_diagnostics(source: str, stderr: str) -> list[dict[str, Any]]:
    lines = source.splitlines()
    diagnostics: list[dict[str, Any]] = []
    output = stderr.splitlines()

    index = 0
    while index < len(output):
        match = _DIAGNOSTIC_HEADER.match(output[index])
        if not match:
            index += 1
            continue

        line_number = max(1, int(match.group(1)))
        column = max(1, int(match.group(2)))
        message = match.group(3)

        caret_length = 1
        if index + 2 < len(output):
            marker = output[index + 2]
            first = marker.find("^")
            if first >= 0:
                count = 0
                while first + count < len(marker) and marker[first + count] == "^":
                    count += 1
                caret_length = max(1, count)

        source_line = lines[line_number - 1] if line_number <= len(lines) else ""
        start_character = byte_column_to_utf16(source_line, column)
        span_character_length = byte_span_to_utf16(
            source_line,
            column,
            caret_length,
        )

        diagnostics.append(
            {
                "range": {
                    "start": {
                        "line": line_number - 1,
                        "character": start_character,
                    },
                    "end": {
                        "line": line_number - 1,
                        "character": start_character + span_character_length,
                    },
                },
                "severity": 1,
                "source": "lune",
                "message": message,
            }
        )
        index += 1

    return diagnostics


def _with_source_file(source: str, action: Any) -> Any:
    path = ""
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            suffix=".lune",
            delete=False,
        ) as handle:
            handle.write(source)
            path = handle.name
        return action(path)
    finally:
        if path:
            try:
                os.unlink(path)
            except FileNotFoundError:
                pass


def diagnostics_for(source: str) -> list[dict[str, Any]]:
    lune = resolve_lune()
    if lune is None:
        return [
            {
                "range": {
                    "start": {"line": 0, "character": 0},
                    "end": {"line": 0, "character": 1},
                },
                "severity": 1,
                "source": "lune",
                "message": "lune executable not found; set LUNE_BIN or add lune to PATH",
            }
        ]

    def check(path: str) -> list[dict[str, Any]]:
        result = subprocess.run(
            [lune, "check", path],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=10,
        )
        if result.returncode == 0:
            return []
        return parse_diagnostics(
            source,
            result.stderr.decode("utf-8", "replace"),
        )

    return _with_source_file(source, check)


def format_text(source: str) -> str:
    lune = resolve_lune()
    if lune is None:
        raise RuntimeError("lune executable not found; set LUNE_BIN or add lune to PATH")

    def format_file(path: str) -> str:
        result = subprocess.run(
            [lune, "fmt", path],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=10,
        )
        if result.returncode != 0:
            message = result.stderr.decode("utf-8", "replace").strip()
            raise RuntimeError(message or "lune fmt failed")
        return Path(path).read_text(encoding="utf-8")

    return _with_source_file(source, format_file)


def document_end(source: str) -> dict[str, int]:
    parts = source.split("\n")
    line = max(0, len(parts) - 1)
    return {
        "line": line,
        "character": utf16_units(parts[-1] if parts else ""),
    }


def read_message() -> dict[str, Any] | None:
    headers: dict[str, str] = {}
    stream = sys.stdin.buffer

    while True:
        line = stream.readline()
        if not line:
            return None
        if line in (b"\r\n", b"\n"):
            break

        name, _, value = line.decode("ascii", "replace").partition(":")
        headers[name.lower()] = value.strip()

    length_text = headers.get("content-length")
    if length_text is None:
        return None

    body = stream.read(int(length_text))
    if len(body) != int(length_text):
        return None

    return json.loads(body.decode("utf-8"))


def write_message(message: dict[str, Any]) -> None:
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii"))
    sys.stdout.buffer.write(body)
    sys.stdout.buffer.flush()


def publish_diagnostics(uri: str, source: str) -> None:
    write_message(
        {
            "jsonrpc": "2.0",
            "method": "textDocument/publishDiagnostics",
            "params": {
                "uri": uri,
                "diagnostics": diagnostics_for(source),
            },
        }
    )


def response(request_id: Any, result: Any) -> None:
    write_message(
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "result": result,
        }
    )


def error_response(request_id: Any, code: int, message: str) -> None:
    write_message(
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "error": {
                "code": code,
                "message": message,
            },
        }
    )


def run_server() -> int:
    documents: dict[str, str] = {}
    shutdown_requested = False

    while True:
        message = read_message()
        if message is None:
            return 0

        method = message.get("method")
        params = message.get("params") or {}
        request_id = message.get("id")

        if method == "initialize":
            response(
                request_id,
                {
                    "capabilities": {
                        "textDocumentSync": 1,
                        "documentFormattingProvider": True,
                    },
                    "serverInfo": {
                        "name": "lune-lsp",
                        "version": "0.1.0",
                    },
                },
            )
        elif method == "initialized":
            continue
        elif method == "shutdown":
            shutdown_requested = True
            response(request_id, None)
        elif method == "exit":
            return 0 if shutdown_requested else 1
        elif method == "textDocument/didOpen":
            document = params["textDocument"]
            uri = document["uri"]
            source = document["text"]
            documents[uri] = source
            publish_diagnostics(uri, source)
        elif method == "textDocument/didChange":
            uri = params["textDocument"]["uri"]
            changes = params.get("contentChanges") or []
            if changes:
                source = changes[-1].get("text", "")
                documents[uri] = source
                publish_diagnostics(uri, source)
        elif method == "textDocument/didClose":
            uri = params["textDocument"]["uri"]
            documents.pop(uri, None)
            write_message(
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/publishDiagnostics",
                    "params": {
                        "uri": uri,
                        "diagnostics": [],
                    },
                }
            )
        elif method == "textDocument/formatting":
            uri = params["textDocument"]["uri"]
            source = documents.get(uri, "")
            try:
                formatted = format_text(source)
                response(
                    request_id,
                    [
                        {
                            "range": {
                                "start": {"line": 0, "character": 0},
                                "end": document_end(source),
                            },
                            "newText": formatted,
                        }
                    ],
                )
            except (OSError, subprocess.SubprocessError, RuntimeError) as exc:
                error_response(request_id, -32603, str(exc))
        elif request_id is not None:
            error_response(request_id, -32601, f"unsupported method: {method}")

    return 0


if __name__ == "__main__":
    raise SystemExit(run_server())
