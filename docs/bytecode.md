# Lune bytecode image format

Lune bytecode images use the magic header `LUNEBC01`. Multibyte integers are unsigned little-endian. The format is deliberately small and deterministic so the self-hosted compiler can emit it using only byte-oriented string/list primitives.

After the 8-byte header, the root chunk is encoded recursively.

A chunk contains, in order:

1. `u32 code_count`
2. `code_count` raw opcode/operand bytes
3. `code_count` source spans, each as `u32 start, length, line, column`
4. `u32 constant_count`
5. constants, each as:
   - `u8 tag` (`0` integer, `1` float)
   - `u32 text_length`
   - canonical decimal text bytes
6. `u32 name_count`
7. names, each as `u32 byte_length` followed by raw bytes
8. `u32 function_count`
9. nested functions

A nested function contains:

1. `u16 arity`
2. `u16 upvalue_count`
3. each upvalue as `u8 is_local` plus `u16 index`
4. the function chunk using the chunk encoding above

Numeric constants use decimal text rather than host floating-point bit patterns. Lune's `str(float)` uses enough precision for binary64 round-tripping, which keeps the self-hosted encoder platform-independent and deterministic.

The image format is an internal bootstrap format for now. Compatibility is not promised before Lune 0.1.
