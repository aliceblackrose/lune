# Lune 0.1 standard library and runtime reference

Lune 0.1 keeps its built-in runtime surface intentionally small. The names below are installed as globals in every ordinary script and source module unless otherwise noted.

## Output and process state

### `print(...values)`

Writes values to standard output separated by spaces, appends a newline, and returns `null`.

### `eprint(...values)`

Writes values to standard error separated by spaces, appends a newline, and returns `null`.

### `args`

A list of command-line arguments after the script path. In a source module, `args` remains the process argument list of the root script.

### `env(name)`

Returns the named environment variable as a string, or `null` when it is not present.

### `exit(status)`

Requests process termination with an integer status from 0 through 255. The current script stops executing and the CLI returns that status.

## Type and scalar helpers

### `type(value)`

Returns one of `"null"`, `"bool"`, `"int"`, `"float"`, `"string"`, `"list"`, `"map"`, or `"function"`.

### `len(value)`

Returns the length of a string, list, or map. String lengths are UTF-8 byte counts.

### `str(value)`

Converts scalar values to their textual form. Collection and function values are rejected.

### `int(value)`

Converts an integer, float, or decimal string to an integer. Invalid conversions are runtime errors.

### `float(value)`

Converts an integer, float, or decimal string to a float. Invalid conversions are runtime errors.

### `bool(value)`

Returns Lune truthiness as a boolean. Only `false` and `null` are falsey.

## Lists and byte-oriented helpers

### `push(list, value)`

Appends `value` to `list` in place and returns the same list.

### `pop(list)`

Removes and returns the final element. Popping an empty list is a runtime error.

### `byte_at(text, index)`

Returns the unsigned byte at a UTF-8 byte offset. Invalid indices are runtime errors.

### `slice(value, start, end)`

Returns the half-open `[start, end)` slice of a string by byte offset or a list by element index.

### `bytes(values)`

Builds a raw-byte string from a list of integers in the range 0 through 255. Lune strings may contain NUL bytes.

## String helpers

### `contains(text, needle)`

Returns whether `needle` occurs as a byte substring of `text`.

### `find(text, needle)`

Returns the byte offset of the first occurrence, or `null` when not found.

### `split(text, separator)`

Splits a string by a non-empty separator and returns a list of strings.

### `join(items, separator)`

Joins a list of strings with `separator`.

### `format(template, values)`

Replaces each `{}` placeholder in `template` with the corresponding scalar value from `values`. Placeholder and value counts must match.

## Higher-order list helpers

### `each(items, fn)`

Calls `fn(item)` for each element in order and returns the original list.

### `map(items, fn)`

Returns a new list containing `fn(item)` for every element.

### `filter(items, fn)`

Returns a new list containing elements for which `fn(item)` is truthy.

### `reduce(items, initial, fn)`

Evaluates `fn(accumulator, item)` from left to right and returns the final accumulator.

Callbacks use ordinary Lune call semantics, including exact arity for Lune functions and normal runtime error propagation.

## JSON

### `json_parse(text)`

Parses strict JSON. Objects become maps, arrays become lists, strings remain strings, booleans and null map directly, and integral JSON numbers remain integers when they fit signed 64-bit.

### `json_stringify(value)`

Serializes null, booleans, finite numbers, strings, lists, and maps. Functions and cyclic collection graphs are runtime errors.

### `import("json")`

Returns a native module map with `parse` and `stringify` entries corresponding to `json_parse` and `json_stringify`.

## Files and paths

### `read_file(path)`

Reads the entire file and returns its bytes as a Lune string.

### `write_file(path, data)`

Replaces a file with the bytes in `data` and returns the number of bytes written.

### `path_join(a, b)`

Joins two slash-separated path components. An absolute `b` replaces `a`.

### `path_base(path)`

Returns the final path component.

### `path_dir(path)`

Returns the directory portion, or `"."` when there is no separator.

## Child processes

### `exec(program, arguments)`

Executes `program` directly with a list of string arguments. No shell is inserted implicitly.

The result is a map:

```lune
{
    status: 0,
    stdout: "...",
    stderr: "...",
}
```

On POSIX, a process terminated by a signal reports `128 + signal` as its status.

## Modules

### `import(specifier)`

Loads a module and returns its final value.

Path-like specifiers resolve relative to the importing file. Extensionless source imports gain `.lune`; extensionless imports originating from generated `.lbc` compiler modules resolve to sibling `.lbc` images. Canonical file paths are cached, so a module executes at most once per VM run.

Bare specifiers name native modules. Lune 0.1 currently provides only `"json"`.

Cyclic imports are runtime errors.

## Error behavior

All helpers validate argument count and value kinds. Invalid arguments, failed conversions, invalid indices, file/process failures, and unsupported values produce ordinary runtime errors with source context and stack traces where applicable.

The compiler-facing byte helpers are public in 0.1 because the compiler is written in Lune. They are intentionally low-level and may be complemented by higher-level library modules in later releases.
