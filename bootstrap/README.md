# Bootstrap frontend

This directory contains the temporary native frontend used to bootstrap Lune.

It is intentionally written in dependency-free C17. The lexer, parser, and bytecode compiler in this directory are not intended to remain the production frontend: Milestone 5 replaces them with implementations written in Lune.

The native VM/runtime can remain native.

## Build

```sh
make
make test
```

Inspect tokens with:

```sh
./bootstrap/build/lune-bootstrap lex examples/hello.lune
```
