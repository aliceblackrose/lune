#include "compiler.h"
#include "object.h"
#include "parser.h"
#include "vm.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    int count;
} Diagnostics;

typedef struct {
    bool ok;
    LuneValue value;
    LuneVM *vm;
    int diagnostics;
} EvalResult;

static int failures = 0;

static void diagnostic(
    void *context,
    LuneSpan span,
    const char *message
) {
    (void)span;
    (void)message;
    ((Diagnostics *)context)->count++;
}

static void fail(
    const char *name,
    const char *message
) {
    fprintf(
        stderr,
        "FAIL %s: %s\n",
        name,
        message
    );
    failures++;
}

static EvalResult eval_with_args(
    const char *source,
    int process_argc,
    const char *const *process_argv
) {
    Diagnostics diagnostics = {0};

    LuneParser parser;
    lune_parser_init(
        &parser,
        source,
        strlen(source),
        diagnostic,
        &diagnostics
    );

    LuneAst *ast =
        lune_parse_program(&parser);

    if (
        ast == NULL ||
        parser.had_error
    ) {
        lune_ast_free(ast);

        return (EvalResult){
            .ok = false,
            .diagnostics =
                diagnostics.count,
        };
    }

    LuneChunk chunk;
    lune_chunk_init(&chunk);

    bool ok = lune_compile(
        ast,
        source,
        &chunk,
        diagnostic,
        &diagnostics
    );

    LuneVM *vm = NULL;
    LuneValue value =
        lune_value_null();

    if (ok) {
        vm = lune_vm_new(
            diagnostic,
            &diagnostics
        );

        if (vm != NULL) {
            lune_vm_set_process_args(
                vm,
                process_argc,
                process_argv
            );
            lune_vm_set_gc_stress(
                vm, true
            );
            ok = lune_vm_run(
                vm,
                &chunk,
                &value
            );
        } else {
            ok = false;
        }
    }

    lune_chunk_free(&chunk);
    lune_ast_free(ast);

    return (EvalResult){
        .ok = ok,
        .value = value,
        .vm = vm,
        .diagnostics =
            diagnostics.count,
    };
}

static EvalResult eval(
    const char *source
) {
    return eval_with_args(
        source, 0, NULL
    );
}

static void close_result(
    EvalResult *result
) {
    lune_vm_free(result->vm);
    result->vm = NULL;
}

static void expect_int(
    const char *name,
    const char *source,
    int64_t expected
) {
    EvalResult result = eval(source);

    if (
        !result.ok ||
        result.value.kind !=
            LUNE_VALUE_INT ||
        result.value.as.integer !=
            expected
    ) {
        fail(name, "unexpected result");
    }

    close_result(&result);
}

static void expect_bool(
    const char *name,
    const char *source,
    bool expected
) {
    EvalResult result = eval(source);

    if (
        !result.ok ||
        result.value.kind !=
            LUNE_VALUE_BOOL ||
        result.value.as.boolean !=
            expected
    ) {
        fail(name, "unexpected result");
    }

    close_result(&result);
}

static void expect_null(
    const char *name,
    const char *source
) {
    EvalResult result = eval(source);

    if (
        !result.ok ||
        result.value.kind !=
            LUNE_VALUE_NULL
    ) {
        fail(name, "expected null");
    }

    close_result(&result);
}

static void expect_string(
    const char *name,
    const char *source,
    const char *expected,
    size_t expected_length
) {
    EvalResult result = eval(source);

    if (
        !result.ok ||
        result.value.kind !=
            LUNE_VALUE_OBJ ||
        !lune_obj_is_string(
            result.value.as.object
        )
    ) {
        fail(
            name,
            "expected string object"
        );
        close_result(&result);
        return;
    }

    LuneObjString *string =
        (LuneObjString *)
        result.value.as.object;

    if (
        string->length !=
            expected_length ||
        memcmp(
            string->chars,
            expected,
            expected_length
        ) != 0
    ) {
        fail(
            name,
            "unexpected string value"
        );
    }

    close_result(&result);
}

static void expect_error(
    const char *name,
    const char *source
) {
    EvalResult result = eval(source);

    if (
        result.ok ||
        result.diagnostics == 0
    ) {
        fail(name, "expected an error");
    }

    close_result(&result);
}


static void test_manual_gc(void) {
    const char *source =
        "i := 0\n"
        "s := \"\"\n"
        "while i < 200 {\n"
        "  s = \"a\" + \"b\"\n"
        "  i = i + 1\n"
        "}\n"
        "i\n";

    Diagnostics diagnostics = {0};

    LuneParser parser;
    lune_parser_init(
        &parser,
        source,
        strlen(source),
        diagnostic,
        &diagnostics
    );

    LuneAst *ast =
        lune_parse_program(&parser);

    if (
        ast == NULL ||
        parser.had_error
    ) {
        fail(
            "manual-gc",
            "test program did not parse"
        );
        lune_ast_free(ast);
        return;
    }

    LuneChunk chunk;
    lune_chunk_init(&chunk);

    bool ok = lune_compile(
        ast,
        source,
        &chunk,
        diagnostic,
        &diagnostics
    );

    LuneVM *vm = NULL;
    LuneValue result =
        lune_value_null();

    if (ok) {
        vm = lune_vm_new(
            diagnostic,
            &diagnostics
        );

        ok = vm != NULL &&
            lune_vm_run(
                vm,
                &chunk,
                &result
            );
    }

    if (
        !ok ||
        result.kind !=
            LUNE_VALUE_INT ||
        result.as.integer != 200
    ) {
        fail(
            "manual-gc",
            "program did not execute"
        );
    } else {
        size_t before =
            lune_vm_heap_bytes(vm);

        lune_vm_collect_garbage(vm);

        size_t after =
            lune_vm_heap_bytes(vm);

        if (after >= before) {
            fail(
                "manual-gc",
                "collector did not reclaim unreachable objects"
            );
        }
    }

    lune_vm_free(vm);
    lune_chunk_free(&chunk);
    lune_ast_free(ast);
}

int main(void) {
    expect_int(
        "arithmetic",
        "x := 2 + 3 * 4\n"
        "x = x + 1\n"
        "x\n",
        15
    );

    expect_int(
        "if",
        "x := 3\n"
        "if x > 2 {\n"
        "  x * 10\n"
        "} else {\n"
        "  0\n"
        "}\n",
        30
    );

    expect_int(
        "while",
        "x := 0\n"
        "while x < 5 {\n"
        "  x = x + 1\n"
        "}\n"
        "x\n",
        5
    );

    expect_int(
        "scope",
        "x := 1\n"
        "if true {\n"
        "  x := 5\n"
        "  x\n"
        "}\n"
        "x\n",
        1
    );

    expect_bool(
        "and-short-circuit",
        "false and missing\n",
        false
    );

    expect_bool(
        "or-short-circuit",
        "true or missing\n",
        true
    );

    expect_string(
        "string-concat",
        "\"hello, \" + \"lune\"\n",
        "hello, lune",
        strlen("hello, lune")
    );

    expect_string(
        "unicode-escape",
        "\"rocket: \\u{1F680}\"\n",
        "rocket: \xF0\x9F\x9A\x80",
        sizeof(
            "rocket: \xF0\x9F\x9A\x80"
        ) - 1
    );

    expect_int(
        "list-index-mutation",
        "items := [1, 2, 3]\n"
        "items[1] = 9\n"
        "items[1]\n",
        9
    );

    expect_string(
        "map-member",
        "user := {name: \"Alice\"}\n"
        "user.name\n",
        "Alice",
        5
    );

    expect_string(
        "map-mutation",
        "user := {name: \"Alice\"}\n"
        "user.name = \"Bob\"\n"
        "user[\"name\"]\n",
        "Bob",
        3
    );

    expect_int(
        "map-index-create",
        "m := {}\n"
        "m[\"score\"] = 42\n"
        "m.score\n",
        42
    );

    expect_null(
        "missing-map-key",
        "m := {}\n"
        "m.missing\n"
    );

    expect_bool(
        "string-equality",
        "\"lu\" + \"ne\" == \"lune\"\n",
        true
    );

    expect_bool(
        "list-identity",
        "items := []\n"
        "items == items\n",
        true
    );

    expect_bool(
        "distinct-list-identity",
        "[] == []\n",
        false
    );

    expect_int(
        "simple-function",
        "add := fn(a, b) => a + b\n"
        "add(2, 3)\n",
        5
    );

    expect_int(
        "block-function",
        "f := fn(x) => {\n"
        "  y := x * 2\n"
        "  y + 1\n"
        "}\n"
        "f(4)\n",
        9
    );

    expect_int(
        "closure-mutation",
        "make := fn(start) => {\n"
        "  n := start\n"
        "  fn() => {\n"
        "    n = n + 1\n"
        "    n\n"
        "  }\n"
        "}\n"
        "next := make(10)\n"
        "next()\n"
        "next()\n",
        12
    );

    expect_int(
        "shared-upvalue",
        "make := fn() => {\n"
        "  n := 0\n"
        "  {\n"
        "    inc: fn() => {\n"
        "      n = n + 1\n"
        "      n\n"
        "    },\n"
        "    get: fn() => n,\n"
        "  }\n"
        "}\n"
        "pair := make()\n"
        "pair.inc()\n"
        "pair.inc()\n"
        "pair.get()\n",
        2
    );

    expect_int(
        "nested-upvalue",
        "outer := fn(x) => "
        "fn() => fn() => x\n"
        "outer(7)()()\n",
        7
    );

    expect_int(
        "global-recursion",
        "fact := fn(n) => "
        "if n <= 1 { 1 } "
        "else { n * fact(n - 1) }\n"
        "fact(6)\n",
        720
    );

    expect_int(
        "local-recursion",
        "run := fn(n) => {\n"
        "  fact := fn(x) => "
        "if x <= 1 { 1 } "
        "else { x * fact(x - 1) }\n"
        "  fact(n)\n"
        "}\n"
        "run(5)\n",
        120
    );

    expect_int(
        "captured-slot-not-reused",
        "make := fn() => {\n"
        "  f := null\n"
        "  if true {\n"
        "    x := 41\n"
        "    f = fn() => x\n"
        "  }\n"
        "  if true {\n"
        "    y := 99\n"
        "    y\n"
        "  }\n"
        "  f\n"
        "}\n"
        "make()()\n",
        41
    );

    expect_null(
        "native-print",
        "print(\"native print ok\")\n"
    );

    expect_string(
        "type-string",
        "type([1, 2, 3])\n",
        "list",
        4
    );

    expect_int(
        "len-string-bytes",
        "len(\"rocket: \\u{1F680}\")\n",
        12
    );

    expect_int(
        "len-list",
        "len([1, 2, 3, 4])\n",
        4
    );

    expect_int(
        "len-map",
        "len({a: 1, b: 2})\n",
        2
    );

    expect_string(
        "str-int",
        "str(-42)\n",
        "-42",
        3
    );

    expect_int(
        "int-string",
        "int(\"12345\")\n",
        12345
    );

    {
        EvalResult converted =
            eval("float(\"3.5\")\n");

        if (
            !converted.ok ||
            converted.value.kind !=
                LUNE_VALUE_FLOAT ||
            converted.value.as.floating !=
                3.5
        ) {
            fail(
                "float-string",
                "unexpected result"
            );
        }

        close_result(&converted);
    }

    expect_bool(
        "bool-null",
        "bool(null)\n",
        false
    );

    expect_null(
        "env-missing",
        "env(\"LUNE_TEST_VARIABLE_THAT_SHOULD_NOT_EXIST_9F5F6A\")\n"
    );

    {
        const char *arguments[] = {
            "alpha",
            "beta",
        };

        EvalResult argument_result =
            eval_with_args(
                "args[1]\n",
                2,
                arguments
            );

        if (
            !argument_result.ok ||
            argument_result.value.kind !=
                LUNE_VALUE_OBJ ||
            !lune_obj_is_string(
                argument_result
                    .value.as.object
            )
        ) {
            fail(
                "process-args",
                "expected string argument"
            );
        } else {
            LuneObjString *string =
                (LuneObjString *)
                    argument_result
                        .value.as.object;

            if (
                string->length != 4 ||
                memcmp(
                    string->chars,
                    "beta",
                    4
                ) != 0
            ) {
                fail(
                    "process-args",
                    "unexpected argument"
                );
            }
        }

        close_result(
            &argument_result
        );
    }

    {
        EvalResult exit_result =
            eval(
                "exit(23)\n"
                "missing\n"
            );

        int status = -1;

        if (
            !exit_result.ok ||
            !lune_vm_exit_status(
                exit_result.vm,
                &status
            ) ||
            status != 23
        ) {
            fail(
                "exit-status",
                "exit status was not preserved"
            );
        }

        close_result(&exit_result);
    }

    expect_error(
        "len-type-error",
        "len(1)\n"
    );

    expect_error(
        "int-parse-error",
        "int(\"12x\")\n"
    );

    expect_error(
        "exit-range-error",
        "exit(300)\n"
    );

    expect_error(
        "wrong-arity",
        "f := fn(x) => x\n"
        "f()\n"
    );

    expect_error(
        "call-non-function",
        "x := 1\n"
        "x()\n"
    );

    expect_error(
        "unknown-assignment",
        "missing = 1\n"
    );

    expect_error(
        "redeclaration",
        "x := 1\n"
        "x := 2\n"
    );

    expect_error(
        "list-range",
        "items := [1]\n"
        "items[1]\n"
    );

    expect_error(
        "map-key-type",
        "m := {}\n"
        "m[1]\n"
    );

    expect_error(
        "mixed-string-add",
        "\"x\" + 1\n"
    );

    expect_bool(
        "gc-cycle-root",
        "m := {}\n"
        "m.self = m\n"
        "i := 0\n"
        "while i < 30 {\n"
        "  temp := {root: m}\n"
        "  i = i + 1\n"
        "}\n"
        "m.self == m\n",
        true
    );

    test_manual_gc();

    if (failures != 0) {
        fprintf(
            stderr,
            "%d VM test(s) failed\n",
            failures
        );
        return 1;
    }

    puts("VM tests passed");
    return 0;
}
