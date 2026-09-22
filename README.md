# Lune

Lune is an experimental small, modern scripting language designed around a minimal composable core, fast bytecode execution, and an eventually self-hosted compiler.

Current status: **language design / bootstrap**.

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
