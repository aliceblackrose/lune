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

- [ ] Replace the permissive token-stream grammar with actual language syntax.
- [ ] Define newline-separated statements with no required semicolons.
- [ ] Define comments.
- [ ] Define identifiers and UTF-8 source handling.
- [ ] Define literals:
  - [ ] `null`
  - [ ] booleans
  - [ ] numbers
  - [ ] strings
  - [ ] lists
  - [ ] maps
- [ ] Define `:=` as declaration.
- [ ] Define `=` as assignment to an existing binding.
- [ ] Define arithmetic and comparison operators.
- [ ] Define short-circuit `and`, `or`, and `not`.
- [ ] Define function expressions: `fn(a, b) => expr`.
- [ ] Define block-bodied functions.
- [ ] Define expression-oriented `if / else`.
- [ ] Define `while` as the primitive loop.
- [ ] Define calls, indexing, and member access.

### Semantics

- [ ] Specify lexical scope.
- [ ] Specify closure capture and mutation.
- [ ] Specify truthiness.
- [ ] Specify numeric behavior.
- [ ] Specify equality.
- [ ] Specify block result values.
- [ ] Specify evaluation order.
- [ ] Specify runtime error behavior.
- [ ] Decide whether map member access is equivalent to string-key indexing.

### Deliverables

- [ ] `docs/language.md`
- [ ] Example programs under `examples/`
- [ ] Parser tests for every accepted and rejected syntax form.

**Done when:** the syntax and semantics of the core language can be described without referring to implementation details.

---

# Milestone 1 — Bootstrap frontend

**Goal:** parse real Lune programs with no parser-generator dependency.

ANTLR may be used as an early reference/prototyping aid, but it is not part of the long-term runtime.

### Lexer

- [ ] Hand-written lexer.
- [ ] Source spans on every token.
- [ ] Useful lexical diagnostics.
- [ ] Escape sequences in strings.
- [ ] Newlines preserved where syntactically relevant.

### Parser

- [ ] Hand-written parser.
- [ ] Pratt parser for expressions.
- [ ] Parse declarations and assignment.
- [ ] Parse blocks.
- [ ] Parse functions.
- [ ] Parse `if / else`.
- [ ] Parse `while`.
- [ ] Parse list and map literals.
- [ ] Parse calls, indexing, and member access.
- [ ] Recover from common syntax errors well enough to report more than one diagnostic.

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

**Done when:** representative Lune scripts parse into a compact AST with deterministic diagnostics.

---

# Milestone 2 — Bytecode VM

**Goal:** execute Lune through bytecode instead of a long-term tree-walk interpreter.

Start with a stack VM unless benchmarks demonstrate a reason to change.

### Runtime values

- [ ] `null`
- [ ] boolean
- [ ] integer / numeric representation
- [ ] floating-point representation if separate from integers
- [ ] string
- [ ] list
- [ ] map
- [ ] function
- [ ] closure
- [ ] native function

### VM

- [ ] Constant pool.
- [ ] Operand stack.
- [ ] Call frames.
- [ ] Locals.
- [ ] Globals.
- [ ] Jumps.
- [ ] Function calls.
- [ ] Closures and upvalues.
- [ ] Indexing.
- [ ] Member access.
- [ ] Runtime errors with source locations.

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

- [ ] Compile literals.
- [ ] Compile expressions.
- [ ] Compile declarations and assignment.
- [ ] Compile short-circuit operators.
- [ ] Compile conditionals.
- [ ] Compile loops.
- [ ] Compile functions.
- [ ] Compile closures.
- [ ] Compile collections.

**Done when:** non-trivial scripts execute exclusively through Lune bytecode.

---

# Milestone 3 — Memory and performance foundations

**Goal:** make the runtime suitable for real scripts without complicating the language.

### Memory management

- [ ] Automatic garbage collection.
- [ ] Start with tracing mark-and-sweep.
- [ ] Correctly trace closures, lists, maps, strings, and VM roots.
- [ ] Stress-GC test mode.
- [ ] Memory-safety regression tests.

### Representation

- [ ] Keep common scalar values allocation-free.
- [ ] Avoid allocating ordinary integer arithmetic results where practical.
- [ ] Intern strings only if profiling justifies it.
- [ ] Keep object layouts simple before attempting advanced representation tricks.

### Benchmark baseline

Track at least:

- [ ] process startup time
- [ ] lexer/parser throughput
- [ ] bytecode compilation time
- [ ] function-call overhead
- [ ] tight integer loop
- [ ] list iteration
- [ ] map lookup
- [ ] string-heavy workload
- [ ] GC-heavy workload

Do not add JIT compilation at this stage.

**Done when:** the VM has predictable memory behavior and a reproducible benchmark suite.

---

# Milestone 4 — Useful scripting runtime

**Goal:** make Lune useful outside language tests.

Keep native APIs primitive. Implement higher-level behavior in Lune when practical.

### Core built-ins

- [ ] `print`
- [ ] `type`
- [ ] `len`
- [ ] conversions
- [ ] process arguments
- [ ] environment access
- [ ] exit status

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
