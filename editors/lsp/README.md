# Lune language server

`lune_lsp.py` is a small dependency-free Language Server Protocol adapter for Lune 0.1.

It intentionally delegates language semantics to the production CLI:

- diagnostics call `lune check` on the current document contents;
- document formatting calls `lune fmt`;
- UTF-8 byte-oriented Lune source columns are translated to UTF-16 LSP positions.

Supported LSP features:

- `initialize` / `shutdown` / `exit`;
- full-document text synchronization;
- diagnostics on open and change;
- whole-document formatting.

## Run

Install or build Lune first, then start:

```sh
python3 editors/lsp/lune_lsp.py
```

The server finds `lune` in this order:

1. `LUNE_BIN`;
2. `lune` on `PATH`;
3. the repository development binary at `bootstrap/build/lune`.

For an editor that accepts an LSP command, configure:

```text
python3 /path/to/lune/editors/lsp/lune_lsp.py
```

Set `LUNE_BIN=/path/to/lune` when the executable is not on `PATH`.

The server uses only the Python standard library. Python is tooling-only and is not a Lune runtime dependency.
