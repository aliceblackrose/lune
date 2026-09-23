# Changelog

All notable changes to Lune are documented here.

## 0.1.0 — 2026-09-23

Lune 0.1 is the first self-hosted release.

### Language

- Expression-oriented core with mutable lexical bindings using `:=` and `=`.
- Integers, floats, strings, booleans, null, lists, maps, functions, closures, conditionals, and `while`.
- Newline-separated statements with no semicolons.
- Short-circuit `and`, `or`, and unary `not`.
- Index and member access with mutable list/map assignment.
- Cached file modules through `import()`, plus the native `json` module.
- Defined truthiness, numeric overflow, equality, ordering, evaluation order, and runtime error behavior.

### Compiler and VM

- Lune-written lexer, Pratt parser, compiler, deterministic bytecode encoder, and compiler driver.
- Production source execution uses the self-hosted compiler; the C frontend remains only as the bootstrap seed.
- Deterministic `LUNEBC01` bytecode images with exact stage-1/stage-2 bootstrap comparison.
- Native C17 stack VM with lexical closures and open/closed upvalues.
- Tracing mark-and-sweep garbage collector with stress-GC tests.
- Source-module compilation through the same self-hosted compiler path.
- Path-aware runtime error state and cross-module stack traces.

### Runtime library

- Output, type, conversion, environment, and process-exit helpers.
- Mutable list helpers and byte-oriented compiler primitives.
- String search/split/join/format helpers.
- Higher-order `each`, `map`, `filter`, and `reduce`.
- JSON parsing and serialization.
- File, path, and direct child-process primitives.

### CLI and tooling

- Direct script execution with `lune file.lune`.
- Persistent multiline REPL.
- `lune check` syntax/compiler validation.
- `lune compile` deterministic bytecode emission.
- Direct `.lbc` execution.
- Self-hosted stable `lune fmt` with comment preservation and idempotence tests.
- Compiler source excerpts and exact caret spans.
- Runtime source excerpts and caller stack traces.
- Conservative one-edit name suggestions only when the correction is unique.
- Declarative VS Code language support and TextMate syntax highlighting.
- Relocatable POSIX install layout.

### Quality and reproducibility

- Native lexer, parser, compiler, VM, GC, runtime, and production integration tests.
- Self-hosted lexer/parser/compiler/formatter tests.
- Valid/invalid syntax corpus and executable examples.
- Exact self-host bootstrap reproduction checks.
- AddressSanitizer and UndefinedBehaviorSanitizer CI with leak detection.
- Microbenchmark harness and fresh-process startup benchmark.
