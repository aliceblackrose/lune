# Lune Roadmap

Lune is a small, modern scripting language built around four constraints:

- **Minimal** — a small language core with features that compose instead of overlap.
- **Modern** — lexical scoping, closures, expression-oriented control flow, useful collections, UTF-8 strings, good diagnostics, and strong tooling.
- **Fast** — quick startup and compact bytecode executed by a small native VM.
- **Self-hosted** — the production lexer, parser, and bytecode compiler are eventually written in Lune itself.

The intended architecture is:

```text
source.lune
    ↓
lexer + Pratt parser
    ↓
bytecode compiler
    ↓
.lbc bytecode
    ↓
small native VM
```

The native runtime should stay intentionally small. Higher-level compiler and standard-library code should move into Lune as soon as the language can support it.

---

## Design principles

Before adding syntax, ask:

> Can this be expressed cleanly by composing features Lune already has?

Prefer one general mechanism over several specialized ones.

Examples:

- Functions are values; there is no separate function-declaration construct.
- `if` is an expression; no ternary operator is needed.
- Blocks evaluate to their final expression; explicit `return` is unnecessary in the core language.
- Maps cover objects, records, configuration data, and module namespaces initially.
- Library functions should solve problems before new syntax does.

The initial language should avoid classes, inheritance, generics, macros, annotations, async syntax, pattern matching, operator overloading, and other features until real programs demonstrate a need for them.

---

# Milestone 0 — Language contract

**Goal:** define the smallest coherent Lune that is worth implementing.

### Syntax

- [x] Replace the permissive token-stream grammar with actual language syntax.
- [x] Define newline-separated statements with no required semicolons.
- [x] Define comments.
- [x] Define identifiers and UTF-8 source handling.
- [x] Define literals:
  - [x] `null`
  - [x] booleans
  - [x] numbers
  - [x] strings
  - [x] lists
  - [x] maps
- [x] Define `:=` as declaration.
- [x] Define `=` as assignment to an existing binding.
- [x] Define arithmetic and comparison operators.
- [x] Define short-circuit `and`, `or`, and `not`.
- [x] Define function expressions: `fn(a, b) => expr`.
- [x] Define block-bodied functions.
- [x] Define expression-oriented `if / else`.
- [x] Define `while` as the primitive loop.
- [x] Define calls, indexing, and member access.

### Semantics

- [x] Specify lexical scope.
- [x] Specify closure capture and mutation.
- [x] Specify truthiness.
- [x] Specify numeric behavior.
- [x] Specify equality.
- [x] Specify block result values.
- [x] Specify evaluation order.
- [x] Specify runtime error behavior.
- [x] Decide whether map member access is equivalent to string-key indexing.

### Deliverables

- [x] `docs/language.md`
- [x] Example programs under `examples/`
- [x] Parser tests for every accepted and rejected syntax form.

**Status:** complete.

**Done when:** the syntax and semantics of the core language can be described without referring to implementation details.

---

# Milestone 1 — Bootstrap frontend

**Goal:** parse real Lune programs with no parser-generator dependency.

ANTLR may be used as an early reference/prototyping aid, but it is not part of the long-term runtime.

### Lexer

- [x] Hand-written lexer.
- [x] Source spans on every token.
- [x] Useful lexical diagnostics.
- [x] Escape sequences in strings.
- [x] Newlines preserved where syntactically relevant.

### Parser

- [x] Hand-written parser.
- [x] Pratt parser for expressions.
- [x] Parse declarations and assignment.
- [x] Parse blocks.
- [x] Parse functions.
- [x] Parse `if / else`.
- [x] Parse `while`.
- [x] Parse list and map literals.
- [x] Parse calls, indexing, and member access.
- [x] Recover from common syntax errors well enough to report more than one diagnostic.

### Internal representation

Keep the AST small. Candidate node families:

```text
Literal
Name
Unary
Binary
Call
Index
Member
Function
Block
If
While
Declare
Assign
```

Avoid mirroring every grammar production in the AST.

**Status:** complete. The C17 bootstrap frontend parses the syntax fixtures, validates AST structure and precedence, and recovers across independent syntax errors.

**Done when:** representative Lune scripts parse into a compact AST with deterministic diagnostics.

---

# Milestone 2 — Bytecode VM

**Goal:** execute Lune through bytecode instead of a long-term tree-walk interpreter.

Start with a stack VM unless benchmarks demonstrate a reason to change.

### Current status

The bytecode VM executes the complete v0 core: scalar and collection values, lexical bindings, first-class functions, closures/upvalues, native callables, calls, mutation, short-circuiting, `if`, and `while`.

### Runtime values

- [x] `null`
- [x] boolean
- [x] integer / numeric representation
- [x] floating-point representation if separate from integers
- [x] string
- [x] list
- [x] map
- [x] function
- [x] closure
- [x] native function

### VM

- [x] Constant pool.
- [x] Operand stack.
- [x] Call frames.
- [x] Locals.
- [x] Globals.
- [x] Jumps.
- [x] Function calls.
- [x] Closures and upvalues.
- [x] Indexing.
- [x] Member access.
- [x] Runtime errors with source locations.

A first instruction set should remain compact, roughly along these lines:

```text
CONST
NULL
TRUE
FALSE
POP

GET_LOCAL
SET_LOCAL
GET_UPVALUE
SET_UPVALUE
GET_GLOBAL
SET_GLOBAL

ADD
SUB
MUL
DIV
MOD
EQ
LT
LE
NOT
NEGATE

JUMP
JUMP_FALSE
LOOP

CALL
CLOSURE
RETURN

LIST
MAP
GET_INDEX
SET_INDEX
GET_FIELD
SET_FIELD
```

The exact instruction set is allowed to change while the VM is young.

### Compiler

- [x] Compile literals.
- [x] Compile expressions.
- [x] Compile declarations and assignment.
- [x] Compile short-circuit operators.
- [x] Compile conditionals.
- [x] Compile loops.
- [x] Compile functions.
- [x] Compile closures.
- [x] Compile collections.

**Status:** complete. All repository examples execute through bytecode, and the VM test suite covers functions, recursion, nested closures, shared mutable upvalues, native calls, collections, control flow, and runtime errors.

**Done when:** non-trivial scripts execute exclusively through Lune bytecode.

---

# Milestone 3 — Memory and performance foundations

**Goal:** make the runtime suitable for real scripts without complicating the language.

### Memory management

- [x] Automatic garbage collection.
- [x] Start with tracing mark-and-sweep.
- [x] Correctly trace closures, lists, maps, strings, and VM roots.
- [x] Stress-GC test mode.
- [x] Memory-safety regression tests.

### Representation

- [x] Keep common scalar values allocation-free.
- [x] Avoid allocating ordinary integer arithmetic results where practical.
- [x] Defer string interning until profiling demonstrates a need.
- [x] Keep object layouts simple before attempting advanced representation tricks.

### Benchmark baseline

Track at least:

- [x] process startup time
- [x] lexer/parser throughput
- [x] bytecode compilation time
- [x] function-call overhead
- [x] tight integer loop
- [x] list indexing / iteration-style access
- [x] map lookup
- [x] string-heavy workload
- [x] GC-heavy workload

Do not add JIT compilation at this stage.

**Status:** complete. The runtime uses tracing mark-and-sweep GC, VM tests run under collect-on-every-allocation stress mode, CI runs ASan/UBSan with leak detection, and `make bench` provides a reproducible performance baseline.

**Done when:** the VM has predictable memory behavior and a reproducible benchmark suite.

---

# Milestone 4 — Useful scripting runtime

**Goal:** make Lune useful outside language tests.

Keep native APIs primitive. Implement higher-level behavior in Lune when practical.

### Core built-ins

- [x] `print`
- [x] `type`
- [x] `len`
- [x] conversions (`str`, `int`, `float`, `bool`)
- [x] process arguments (`args`)
- [x] environment access (`env`)
- [x] exit status (`exit`)

### Files and processes

- [ ] read a file
- [ ] write a file
- [ ] path helpers
- [ ] execute a child process
- [ ] capture stdout, stderr, and exit status

### Collections and strings

- [ ] list iteration
- [ ] `map`
- [ ] `filter`
- [ ] `reduce`
- [ ] string splitting/joining
- [ ] searching
- [ ] basic formatting

Prefer library methods/functions over new loop or comprehension syntax.

### Data

- [ ] JSON parse.
- [ ] JSON serialize.

### Modules

Start with the smallest viable model, for example:

```lune
json := import("json")
config := import("./config")
```

- [ ] Module loading.
- [ ] Module cache.
- [ ] Clear module resolution rules.
- [ ] Cyclic-import behavior specified.
- [ ] Native modules and Lune modules exposed through the same user-facing mechanism where practical.

**Done when:** Lune can comfortably implement small command-line automation, file processing, JSON transformation, and process orchestration.

---

# Milestone 5 — Self-hosted compiler

**Goal:** move the language frontend out of the bootstrap implementation.

The native VM remains the trusted execution substrate. The production compiler becomes Lune code.

### Required language capabilities

Before beginning the port, Lune must be able to efficiently manipulate:

- [ ] strings
- [ ] lists
- [ ] maps
- [ ] byte buffers or equivalent
- [ ] files
- [ ] functions and closures

### Port

Create:

```text
compiler/
    lexer.lune
    parser.lune
    compiler.lune
```

- [ ] Port the lexer to Lune.
- [ ] Port the Pratt parser to Lune.
- [ ] Port AST representation to Lune.
- [ ] Port bytecode emission to Lune.
- [ ] Compile the Lune compiler with the bootstrap compiler.
- [ ] Run the generated compiler on the compiler source itself.

### Bootstrap verification

The bootstrap chain should become:

```text
native/bootstrap compiler
        ↓
compiler/*.lune
        ↓
compiler.lbc
        ↓
compiler/*.lune
        ↓
compiler2.lbc
```

- [ ] Make compiler output deterministic where practical.
- [ ] Compare stage-1 and stage-2 compiler output.
- [ ] Add automated self-hosting/bootstrap tests.

**Done when:** the Lune compiler can compile its own source and the resulting compiler can repeat the process.

---

# Milestone 6 — Remove bootstrap frontend dependency

**Goal:** make the native executable primarily a VM and platform layer.

- [ ] Ship the self-hosted compiler as bytecode or an embedded bytecode image.
- [ ] Remove the native production lexer.
- [ ] Remove the native production parser.
- [ ] Remove the native production bytecode compiler.
- [ ] Keep a minimal bootstrap path documented and reproducible.
- [ ] Keep VM/platform primitives independent of compiler internals.

The production architecture should now resemble:

```text
lune executable
├── VM
├── GC
├── bytecode loader
├── platform primitives
└── embedded self-hosted compiler bytecode
```

**Done when:** ordinary source execution uses the compiler written in Lune.

---

# Milestone 7 — Tooling and 0.1 release

**Goal:** make the language pleasant to use, not merely executable.

### CLI

Target a small command surface:

```text
lune file.lune
lune repl
lune check file.lune
lune fmt file.lune
lune compile file.lune
```

- [ ] Script runner.
- [ ] REPL.
- [ ] Syntax checker.
- [ ] Formatter.
- [ ] Bytecode compilation.
- [ ] Bytecode execution.

### Diagnostics

- [ ] Filename, line, and column.
- [ ] Source excerpt.
- [ ] Highlight exact error span.
- [ ] Human-readable parser errors.
- [ ] Runtime stack traces.
- [ ] Suggestions only when they are unambiguous.

### Developer tooling

- [ ] Stable formatter.
- [ ] Syntax highlighting definition.
- [ ] Editor integration.
- [ ] Language server only after syntax/semantics stabilize.

### Release

- [ ] Language reference.
- [ ] Standard-library reference.
- [ ] Installation instructions.
- [ ] Architecture documentation.
- [ ] Self-hosting documentation.
- [ ] Changelog.
- [ ] Reproducible test suite.
- [ ] Performance baseline.

**Done when:** Lune 0.1 is small, documented, self-hosted, useful for everyday scripts, and has enough tooling to be pleasant to write.

---

# Post-0.1 candidates

These are intentionally **not commitments**.

Only promote a feature when real Lune programs demonstrate that the existing core cannot express the use case cleanly.

- structured error/result ergonomics
- immutable bindings
- iteration protocol
- richer module/package system
- bytecode cache
- generational or incremental GC
- optimized object shapes
- specialized bytecodes
- optional static analysis
- concurrency
- async I/O
- pattern matching
- native extension API
- JIT compilation

---

# Non-goals for the initial language

Lune 0.1 should not attempt to be:

- a systems programming language
- a JavaScript replacement for browsers
- an object-oriented language
- a statically typed language
- a macro language
- a metaprogramming platform
- a JIT-focused runtime
- a large batteries-included application framework

The goal is narrower:

> **Make small scripts concise, predictable, fast to start, fast enough to run, and simple enough that Lune can implement its own compiler.**
