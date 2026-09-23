# Lune 0.1 architecture

Lune 0.1 uses a self-hosted source compiler and a native C17 bytecode VM. The production executable does not link the native lexer, parser, or compiler.

## Production path

The normal source-execution path is:

```text
.lune source
    |
    v
bootstrap/build/lune
    |
    +--> selfhost/stage.lbc
            |
            +--> lexer.lbc
            +--> parser.lbc
            +--> compiler.lbc
            +--> bytecode.lbc
    |
    v
LUNEBC01 bytecode chunk
    |
    v
native VM
```

The production executable links the value/object runtime, garbage collector, bytecode representation and image loader, platform layer, VM, and small CLI orchestration layer.

Source compilation is exposed to the VM through a generic source-compiler callback. Production installs the Lune-written compiler behind that callback. The bootstrap executable installs the native C frontend behind the same interface.

This separation prevents the production runtime from depending on the seed frontend while preserving a reproducible bootstrap path.

## Self-hosted compiler

The compiler lives under `compiler/`:

- `lexer.lune` — tokenization and lexical diagnostics;
- `parser.lune` — Pratt parser and syntax diagnostics;
- `compiler.lune` — lexical resolution and bytecode generation;
- `bytecode.lune` — deterministic `LUNEBC01` image encoding;
- `stage.lune` — compiler driver used for bootstrap and production compilation;
- `formatter.lune` and `fmt.lune` — stable source formatter and driver.

The generated images are stored in `bootstrap/build/selfhost/`.

## Bootstrap path

The seed executable `lune-bootstrap` contains the dependency-free C17 lexer, parser, compiler, and the same native VM/runtime used by production.

Bootstrap proceeds in two stages:

```text
C seed frontend
    |
    v
stage-1 compiler images
    |
    v
stage-1 stage.lbc
    |
    v
stage-2 compiler images
    |
    v
byte-for-byte comparison
```

Exact equality of stage-1 and stage-2 images is the reproducibility criterion.

The seed frontend is a trust anchor and development tool; it is not the ordinary source frontend shipped in the production execution path.

## Bytecode

The VM executes stack bytecode represented by `LuneChunk`. A chunk contains:

- instruction bytes;
- one source span per instruction byte;
- numeric constants;
- interned names;
- nested function metadata and chunks.

Serialized images use the deterministic `LUNEBC01` format documented in [bytecode.md](bytecode.md).

Functions carry lexical upvalue descriptors. At runtime a closure points at its function metadata and captured upvalue cells. Open upvalues reference live frame locals; they are closed when the owning frame exits.

## Runtime values and memory

The VM supports null, booleans, signed 64-bit integers, binary64 floats, strings, lists, maps, closures, upvalues, and native functions.

Heap objects are managed by a tracing mark-and-sweep collector. VM roots include:

- the operand/value stack;
- live call-frame locals and closures;
- globals;
- cached module values;
- open upvalues;
- temporary native roots;
- the most recent result where needed.

GC stress mode is exercised by the native VM tests. CI additionally runs the complete suite under AddressSanitizer and UndefinedBehaviorSanitizer with leak detection enabled.

## Calls and execution frames

Each Lune call frame stores:

- the active chunk and instruction pointer;
- stack base;
- closure, when executing a closure;
- module/source path;
- call-site span;
- fixed local slots and definition state.

The call-site metadata is retained when a runtime error occurs, allowing the production CLI to render path-aware stack traces across ordinary functions and imported source modules.

## Globals and native facilities

Runtime helpers such as `print`, `len`, `import`, JSON, files, paths, and process execution are native functions registered into the global map when a VM run starts.

The public 0.1 runtime surface is documented in [stdlib.md](stdlib.md).

## Modules

`import(specifier)` is implemented by the VM.

File imports are resolved relative to the importing frame's module path and canonicalized before caching. Each module executes once per VM run and exports its final value.

Source modules are compiled through the same source-compiler hook used for the root script. Generated compiler modules can import sibling `.lbc` images, which lets the self-hosted compiler graph run without reintroducing the native frontend.

Native modules use the same import shape. Lune 0.1 includes the `json` native module.

## Diagnostics

The self-hosted frontend produces structured source spans. Compiler diagnostics render:

- filename;
- line and column;
- message;
- source line;
- caret span.

The VM retains structured runtime error state and call frames. The production CLI renders the failing source location plus caller stack frames.

Unknown globals receive a correction only when exactly one currently defined global is within one insertion, deletion, substitution, or adjacent-transposition edit. Ambiguous cases produce no suggestion.

## REPL

The REPL uses incremental VM execution rather than rebuilding the VM per entry. Globals, heap objects, and closures persist across submissions.

Compiled REPL chunks are retained for the lifetime of the session because live closures may reference function metadata owned by earlier chunks. Runtime failures unwind execution state while preserving persistent globals and heap state.

## Formatter

Formatting is implemented in Lune and shipped as bytecode. The native CLI only validates the source and invokes `fmt.lbc`.

The 0.1 formatter deliberately preserves source newlines and `//` comments. It normalizes spacing and brace indentation without becoming a second syntax parser. Idempotence is covered by both self-hosted unit tests and production CLI integration tests.

## Platform layer

OS-specific file, path, environment, temporary-file, and process functionality is isolated behind the C platform layer. Higher compiler/runtime code does not invoke a shell implicitly.

## Build products

A normal build creates:

```text
bootstrap/build/lune
bootstrap/build/selfhost/*.lbc
```

A bootstrap-development build additionally creates:

```text
bootstrap/build/lune-bootstrap
```

The install target places the real executable and bytecode bundle together under `lib/lune` and exposes a relative `bin/lune` symlink. Canonical executable-path resolution makes the installed layout relocatable under a chosen prefix.
