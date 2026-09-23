#!/usr/bin/env python3

from pathlib import Path
import os
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

import lune_lsp  # noqa: E402


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


sample = "double := fn(x) x * 2\n"
sample_stderr = (
    "/tmp/example.lune:1:17: error: expected '=>' after parameters\n"
    "  double := fn(x) x * 2\n"
    "                  ^\n"
)

parsed = lune_lsp.parse_diagnostics(sample, sample_stderr)
require(len(parsed) == 1, "expected one parsed diagnostic")
require(
    parsed[0]["message"] == "expected '=>' after parameters",
    "diagnostic message mismatch",
)
require(parsed[0]["range"]["start"]["line"] == 0, "diagnostic line mismatch")

binary = ROOT / "bootstrap" / "build" / "lune"
require(binary.is_file(), f"production binary is missing: {binary}")
os.environ["LUNE_BIN"] = str(binary)

live = lune_lsp.diagnostics_for(sample)
require(live, "invalid Lune source produced no LSP diagnostics")
require(
    any("expected" in item["message"] for item in live),
    "compiler diagnostic did not survive LSP translation",
)

valid = lune_lsp.diagnostics_for("x := [1, 2]\n")
require(valid == [], "valid Lune source produced diagnostics")

formatted = lune_lsp.format_text("x:=[1,2]\n")
require(formatted == "x := [1, 2]\n", "LSP formatting mismatch")
require(
    lune_lsp.format_text(formatted) == formatted,
    "LSP formatting is not idempotent",
)

unicode_line = "name := \"λ\"\n"
require(
    lune_lsp.byte_column_to_utf16(unicode_line, 10) >= 8,
    "UTF-8 byte column conversion regressed",
)

print("Lune LSP tests passed")
