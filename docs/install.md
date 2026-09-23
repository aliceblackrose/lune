# Installing Lune 0.1

Lune builds from C17 sources and the Lune compiler sources in this repository. The normal build has no third-party runtime dependencies.

## Requirements

For the supported POSIX build path:

- a C17 compiler such as GCC or Clang;
- POSIX `make`;
- standard Unix install utilities for `make install`.

Python 3 is used by the startup benchmark and the optional language-server adapter under `editors/lsp/`; it is not required to build, install, or run the Lune CLI.

The continuous-integration build is validated on Ubuntu. The runtime contains Windows-specific platform paths, but the 0.1 release install procedure is the POSIX path documented here.

## Build from source

From the repository root:

```sh
make
```

The production executable is:

```text
bootstrap/build/lune
```

Its self-hosted compiler and formatter images are generated under:

```text
bootstrap/build/selfhost/
```

Verify the build before installing:

```sh
make test
```

The test suite includes the native bootstrap frontend, self-hosted compiler tests, deterministic stage-1/stage-2 bytecode comparison, production CLI integration tests, formatter idempotence, runtime diagnostics, ASan/UBSan in CI, and source-module execution.

## Install

The default prefix is `/usr/local`:

```sh
sudo make install
```

This installs the real executable and self-hosted bytecode bundle under:

```text
/usr/local/lib/lune/lune
/usr/local/lib/lune/selfhost/*.lbc
```

and creates:

```text
/usr/local/bin/lune -> ../lib/lune/lune
```

The executable resolves its canonical path, so invoking the symlink still locates the adjacent self-hosted compiler images.

Choose another prefix with:

```sh
make install PREFIX="$HOME/.local"
```

Ensure `$HOME/.local/bin` is on `PATH`.

Packaging systems can stage an install with `DESTDIR`:

```sh
make install PREFIX=/usr DESTDIR="$PWD/pkg"
```

## Uninstall

Use the same prefix used during installation:

```sh
sudo make uninstall
```

or:

```sh
make uninstall PREFIX="$HOME/.local"
```

## Compiler selection and flags

Override the compiler or flags in the normal Make style:

```sh
make CC=clang
make CFLAGS='-std=c17 -O3 -Wall -Wextra -Wpedantic -Werror'
```

## Verify an installed CLI

```sh
lune examples/hello.lune
lune check examples/functions.lune
lune compile examples/hello.lune
lune repl
```

`lune compile file.lune` writes a sibling `.lbc` image. `lune fmt file.lune` formats a valid source file in place.
