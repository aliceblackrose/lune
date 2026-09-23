# Self-hosting Lune

Lune's compiler frontend is written in Lune under `compiler/`:

- `lexer.lune`
- `parser.lune`
- `compiler.lune`
- `bytecode.lune` for deterministic `.lbc` encoding
- `formatter.lune` for stable source formatting
- `fmt.lune` as the formatter driver
- `stage.lune` as the bootstrap compiler driver

The native C frontend under `bootstrap/` remains the seed compiler.

## Bootstrap chain

From the repository root:

```sh
make test
```

The `test-bootstrap` target performs this chain:

1. Build the native bootstrap executable.
2. Run `compiler/stage.lune` through the bootstrap VM.
3. Emit stage-1 bytecode images for the lexer, parser, compiler, encoder, formatter, formatter driver, and stage driver.
4. Run the stage-1 `stage.lbc` image.
5. Recompile the same compiler/tooling sources into stage-2 images.
6. Compare every stage-1 and stage-2 image byte-for-byte.
7. Use the generated compiler to compile `examples/hello.lune`.
8. Execute the generated program through `runbc`.

The equality check is intentionally exact. Source traversal, name interning, constant ordering, function ordering, and bytecode encoding are deterministic.

## Bytecode images

The bootstrap image format is documented in [bytecode.md](bytecode.md). The native VM can execute an image directly:

```sh
./bootstrap/build/lune-bootstrap runbc path/to/program.lbc
./bootstrap/build/lune-bootstrap evalbc path/to/program.lbc
```

Bytecode modules import sibling bytecode modules when an extensionless relative import originates from an `.lbc` file. Source modules continue resolving extensionless imports to `.lune`, allowing source and generated compiler graphs to coexist during bootstrap.

## Trust boundary

The bootstrap chain currently trusts:

- the C17 VM/runtime and platform layer;
- the temporary C lexer/parser/compiler used only as the seed;
- the Lune compiler sources.

The production `lune` executable does not link the native lexer, parser, or compiler. Ordinary source execution, `check`, `compile`, source-module imports, and formatter validation all use the Lune-written frontend. The native frontend remains only in `lune-bootstrap` as the reproducible seed path.
