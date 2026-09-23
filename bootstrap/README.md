# Bootstrap and native runtime

This directory contains two native executables with deliberately different roles.

## Production runtime

`build/lune` is the ordinary Lune CLI. It links the native VM, GC, bytecode/image loader, value/object runtime, and platform layer. It does **not** link the C lexer, parser, or bytecode compiler.

The build places the self-hosted compiler bytecode under `build/selfhost/`. The production CLI compiles `.lune` source through those bytecode images, including source modules loaded with `import`.

```sh
make
./build/lune run ../examples/hello.lune
```

## Bootstrap frontend

`build/lune-bootstrap` contains the dependency-free C17 lexer, parser, and bytecode compiler used to reproduce the initial self-hosted compiler images and to debug the bootstrap frontend.

```sh
make bootstrap
./build/lune-bootstrap lex ../examples/hello.lune
./build/lune-bootstrap check ../examples/hello.lune
./build/lune-bootstrap parse ../examples/hello.lune
```

The bootstrap frontend is not part of ordinary production source execution.

## Verification

```sh
make test
```

The test suite verifies the C bootstrap frontend, VM/runtime semantics, deterministic stage-1/stage-2 self-hosting, the core-only production link, production source compilation, and source-module compilation through the self-hosted compiler.
