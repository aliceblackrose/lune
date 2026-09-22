# Lune language contract

This document defines the initial Lune language contract. It describes language behavior, not a particular parser or VM implementation.

Lune is a small, expression-oriented scripting language. Its core is intentionally narrow: values, bindings, functions, conditionals, loops, calls, indexing, and member access.

## Source files

Lune source files use the `.lune` extension and are UTF-8 text.

Version 0 uses ASCII identifiers:

```text
[A-Za-z_][A-Za-z0-9_]*
```

Strings may contain arbitrary Unicode text.

Line comments begin with `//` and continue through the end of the line.

Statements are separated by newlines. Semicolons are not part of the language.

A newline may appear without terminating an expression when the expression is syntactically incomplete, such as after a binary operator, and inside `()`, `[]`, and map literals.

## Values

The core runtime has these value kinds:

- `null`
- boolean
- integer
- float
- string
- list
- map
- function

Native functions are callable runtime values but do not require distinct syntax.

### Null and booleans

```lune
null
true
false
```

Only `false` and `null` are falsey. Every other value is truthy, including `0`, `""`, empty lists, and empty maps.

### Numbers

Integer literals are signed 64-bit integers after unary sign application. Float literals are IEEE-754 binary64 values.

```lune
42
-12
3.14
```

For `+`, `-`, and `*`:

- integer + integer produces an integer when the result fits in signed 64-bit range;
- integer overflow is a runtime error;
- if either operand is a float, both operands are converted to float and the result is a float.

`/` accepts numeric operands and produces a float. Division by zero is a runtime error.

`%` accepts integers only. Modulo by zero is a runtime error.

No implicit string/number coercion is performed.

### Strings

Strings are immutable UTF-8 values delimited by double quotes.

```lune
"hello"
"hello\nworld"
"rocket: \u{1F680}"
```

The initial escape set is:

```text
\"  \\  \n  \r  \t  \0  \u{HEX}
```

String interpolation is not part of the core language.

### Lists

Lists are ordered, mutable, zero-indexed collections.

```lune
names := ["alice", "bob", "charlie"]
first := names[0]
names[1] = "robert"
```

List indices must be integers. An out-of-range index is a runtime error. A trailing comma is allowed.

### Maps

Maps are mutable collections with string keys.

```lune
user := {
    name: "Alice",
    score: 42,
}
```

An identifier key in a map literal is shorthand for a string key, so `{name: "Alice"}` is equivalent to `{"name": "Alice"}`.

Member access is string-key map access:

```lune
user.name
user["name"]
```

These forms are equivalent. Reading a missing map key produces `null`. Assigning a missing map key creates it. Map keys other than strings are not part of version 0.

## Bindings

`:=` creates a mutable binding in the current lexical scope.

```lune
count := 0
```

`=` updates an existing binding:

```lune
count = count + 1
```

Assigning to an unknown identifier is a runtime error. Redeclaring a name with `:=` in the same scope is an error. Declaring the same name in an inner scope shadows the outer binding.

Assignment targets may be names, list/map indexing, or map members:

```lune
count = 1
items[0] = "first"
config.port = 8080
```

Declarations are limited to identifiers in version 0; destructuring is not part of the core language.

## Operators

From lowest to highest precedence:

| Precedence | Operators |
| --- | --- |
| 1 | `or` |
| 2 | `and` |
| 3 | `==`, `!=` |
| 4 | `<`, `<=`, `>`, `>=` |
| 5 | `+`, `-` |
| 6 | `*`, `/`, `%` |
| 7 | unary `-`, `not` |
| 8 | call, index, member access |

Binary arithmetic and comparison operators associate left-to-right. Unary operators associate right-to-left.

### Short-circuit operators

`and` and `or` short-circuit and return operands rather than forcing boolean results.

```lune
port := config.port or 8080
ready and print("ready")
```

For `a and b`, `b` is evaluated only when `a` is truthy. The expression returns `a` when `a` is falsey, otherwise `b`.

For `a or b`, `b` is evaluated only when `a` is falsey. The expression returns `a` when `a` is truthy, otherwise `b`.

`not value` always produces a boolean.

### Equality and ordering

`null`, booleans, numbers, and strings compare by value. Integer and float numeric values may compare equal when they represent the same numeric value.

Lists, maps, and functions compare by identity in version 0. Ordering operators accept numeric operands; other ordering operations are runtime errors.

## Functions

Functions are values. There is no separate function declaration syntax.

```lune
square := fn(x) => x * x
```

Block-bodied function:

```lune
area := fn(w, h) => {
    scale := 2
    w * h * scale
}
```

Parameters are positional. Calls require the exact number of arguments unless a native function explicitly documents otherwise.

Functions close over lexical bindings:

```lune
counter := fn(start) => {
    n := start

    fn() => {
        n = n + 1
        n
    }
}

next := counter(10)
print(next())
print(next())
```

Closures capture bindings, not snapshots of values. Mutation through one closure is visible to other closures that capture the same binding.

There is no `return` keyword in the core language. A function evaluates to the value of its body.

## Blocks

Blocks are bodies of functions, conditionals, and loops. A block creates a lexical scope and evaluates to the result of its final statement. An empty block evaluates to `null`.

Statement results are:

- expression: the expression value;
- declaration: the initialized value;
- assignment: the assigned value;
- `while`: `null`.

Blocks are not general primary expressions in version 0. This keeps `{ ... }` unambiguous with map literals.

## Conditionals

`if` is an expression:

```lune
label := if score >= 70 {
    "pass"
} else {
    "fail"
}
```

`else if` is supported. If no `else` branch is present and the condition is falsey, the expression evaluates to `null`. Only the selected branch is evaluated.

## Loops

`while` is the only primitive loop in version 0:

```lune
i := 0

while i < 10 {
    print(i)
    i = i + 1
}
```

A `while` statement evaluates to `null`. Higher-level iteration belongs in the standard library rather than syntax.

## Calls, indexing, and members

Postfix operations compose left-to-right:

```lune
users[0].name
factory()(value)
config.server.port
```

Calls evaluate the callee first, then arguments from left to right. Binary operands and collection elements also evaluate left to right.

## Errors

Lune does not silently recover from invalid runtime operations. Runtime errors include unknown assignment targets, same-scope redeclarations, calling non-functions, wrong arity, invalid operand types, integer overflow, division/modulo by zero, and invalid or out-of-range list indices.

Diagnostics should include a source location and, once calls exist, a stack trace. Structured exception handling is not part of version 0.

## Core syntax summary

```lune
x := 10
x = x + 1

double := fn(x) => x * 2

kind := if x > 10 {
    "large"
} else {
    "small"
}

while x < 100 {
    x = x * 2
}

values := [1, 2, 3]
config := {host: "localhost", port: 8080}

print(config.host)
```

## Bootstrap runtime globals

The bootstrap scripting runtime currently provides a small set of globals. These are library/runtime facilities rather than new core syntax.

- `print(...values)` prints values separated by spaces and returns `null`.
- `type(value)` returns `"null"`, `"bool"`, `"int"`, `"float"`, `"string"`, `"list"`, `"map"`, or `"function"`.
- `len(value)` returns the length of a string, list, or map. String length is measured in UTF-8 bytes.
- `str(value)` converts scalar values to strings.
- `int(value)` converts an integer, float, or decimal string to an integer.
- `float(value)` converts a number or decimal string to a float.
- `bool(value)` applies Lune truthiness and returns a boolean.
- `args` is a list containing command-line arguments after the script path.
- `env(name)` reads an environment variable and returns a string or `null` when it is absent.
- `exit(status)` terminates the current script run with an integer status from 0 through 255.

Conversion failures and invalid argument types are runtime errors.

Collection helpers:

- `each(items, fn)` calls `fn(item)` for each list element in order and returns the original list.
- `map(items, fn)` returns a new list containing `fn(item)` for each element.
- `filter(items, fn)` returns a new list containing elements whose callback result is truthy.
- `reduce(items, initial, fn)` evaluates `fn(accumulator, item)` from left to right and returns the final accumulator.

Callbacks may be Lune closures or native functions. Callback arity and runtime errors use the same call semantics as ordinary Lune calls.

String helpers:

- `contains(text, needle)` tests whether a byte substring is present.
- `find(text, needle)` returns the byte offset of the first match, or `null` when no match exists.
- `split(text, separator)` returns a list of strings and rejects an empty separator.
- `join(items, separator)` joins a list of strings.
- `format(template, values)` replaces each `{}` placeholder with the corresponding scalar value from a list. The placeholder count must match the value count.

String offsets and lengths are byte-oriented in the bootstrap runtime so UTF-8 source/compiler code can explicitly reason about encoded data.

File/process primitives:

- `read_file(path)` reads the complete file and returns its bytes as a Lune string.
- `write_file(path, data)` replaces a file with the supplied string bytes and returns the number of bytes written.
- `path_join(a, b)` joins two slash-separated path components; an absolute second path replaces the first.
- `path_base(path)` returns the final path component.
- `path_dir(path)` returns the directory portion, or `"."` when no separator is present.
- `exec(program, arguments)` executes a program directly with a list of string arguments. It does not invoke a shell implicitly. The result is a map with integer `status` plus string `stdout` and `stderr` fields. A process terminated by a signal reports `128 + signal` as its status on POSIX platforms.


## Intentionally absent from version 0

Version 0 has no semicolons, `return`, `const`/`let`/`var`, classes, static types, generics, exceptions, destructuring, pattern matching, macros, async syntax, user-defined operators, dedicated `for` loop, or string interpolation.

These features are not reserved for future inclusion. Additions should be justified by real Lune programs that cannot be expressed cleanly through the existing core and library code.
