# Lune

Lune is an experimental small, modern scripting language designed around a minimal composable core, fast bytecode execution, and an eventually self-hosted compiler.

Current status: **executable bytecode runtime / scripting APIs in progress**.

```lune
greet := fn(name) => "hello, " + name
user := {name: "world"}
print(greet(user.name))
```

See [the language contract](docs/language.md) for current semantics and [the roadmap](ROADMAP.md) for implementation milestones.

The ANTLR grammar under `grammar/` is a reference/prototyping grammar. The long-term frontend is intended to be a hand-written lexer and Pratt parser so the compiler can be self-hosted.

## Bootstrap development

The current bootstrap frontend is dependency-free C17.

```sh
make
make test
./bootstrap/build/lune-bootstrap lex examples/hello.lune
./bootstrap/build/lune-bootstrap check examples/hello.lune
./bootstrap/build/lune-bootstrap parse examples/hello.lune
```

## Executing Lune

The bootstrap VM executes the complete v0 language core, including functions, closures, mutable collections, control flow, and native calls.

```sh
./bootstrap/build/lune-bootstrap run examples/scalars.lune
./bootstrap/build/lune-bootstrap eval examples/scalars.lune
```

`eval` prints the script's final value and is intended as a bootstrap/debugging command. Runtime globals currently include `print`, `type`, `len`, scalar conversions, `args`, `env`, `exit`, higher-order list helpers (`each`, `map`, `filter`, `reduce`), string helpers (`split`, `join`, `find`, `contains`, `format`), file/path helpers, and direct child-process execution via `exec`. Script arguments may follow the file path, for example `lune-bootstrap run script.lune one two`.
