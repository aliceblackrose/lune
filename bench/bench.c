#include "ast.h"
#include "bytecode.h"
#include "compiler.h"
#include "parser.h"
#include "value.h"
#include "vm.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int count;
} Diagnostics;

static void diagnostic(
    void *context,
    LuneSpan span,
    const char *message
) {
    (void)span;
    (void)message;
    ((Diagnostics *)context)->count++;
}

static double seconds_now(void) {
    return (double)clock() /
        (double)CLOCKS_PER_SEC;
}

static char *make_frontend_source(
    size_t lines,
    size_t *length
) {
    size_t capacity = lines * 32 + 1;
    char *source = malloc(capacity);
    if (source == NULL) return NULL;

    size_t used = 0;

    for (size_t i = 0; i < lines; i++) {
        int written = snprintf(
            source + used,
            capacity - used,
            "v%zu := %zu + %zu * 2\n",
            i,
            i,
            i + 1
        );

        if (
            written < 0 ||
            (size_t)written >=
                capacity - used
        ) {
            free(source);
            return NULL;
        }

        used += (size_t)written;
    }

    *length = used;
    return source;
}

static LuneAst *parse_source(
    const char *source,
    size_t length,
    Diagnostics *diagnostics
) {
    LuneParser parser;

    lune_parser_init(
        &parser,
        source,
        length,
        diagnostic,
        diagnostics
    );

    LuneAst *ast =
        lune_parse_program(&parser);

    if (
        ast == NULL ||
        parser.had_error
    ) {
        lune_ast_free(ast);
        return NULL;
    }

    return ast;
}

static bool compile_source(
    const char *source,
    size_t length,
    LuneAst **ast_out,
    LuneChunk *chunk,
    Diagnostics *diagnostics
) {
    LuneAst *ast =
        parse_source(
            source,
            length,
            diagnostics
        );

    if (ast == NULL) {
        return false;
    }

    lune_chunk_init(chunk);

    if (!lune_compile(
        ast,
        source,
        chunk,
        diagnostic,
        diagnostics
    )) {
        lune_chunk_free(chunk);
        lune_ast_free(ast);
        return false;
    }

    *ast_out = ast;
    return true;
}

static bool benchmark_frontend(void) {
    size_t length = 0;
    char *source =
        make_frontend_source(
            4000, &length
        );

    if (source == NULL) {
        fputs(
            "benchmark: out of memory\n",
            stderr
        );
        return false;
    }

    const int iterations = 20;
    Diagnostics diagnostics = {0};

    double start = seconds_now();

    for (
        int i = 0;
        i < iterations;
        i++
    ) {
        LuneAst *ast =
            parse_source(
                source,
                length,
                &diagnostics
            );

        if (ast == NULL) {
            free(source);
            return false;
        }

        lune_ast_free(ast);
    }

    double elapsed =
        seconds_now() - start;

    double mib =
        ((double)length *
            (double)iterations) /
        (1024.0 * 1024.0);

    printf(
        "%-24s %10.2f MiB/s\n",
        "frontend parse",
        elapsed > 0.0
            ? mib / elapsed
            : 0.0
    );

    free(source);
    return diagnostics.count == 0;
}

static bool benchmark_compile(void) {
    size_t length = 0;
    char *source =
        make_frontend_source(
            1500, &length
        );

    if (source == NULL) {
        return false;
    }

    Diagnostics diagnostics = {0};

    LuneAst *ast =
        parse_source(
            source,
            length,
            &diagnostics
        );

    if (ast == NULL) {
        free(source);
        return false;
    }

    const int iterations = 20;
    double start = seconds_now();

    for (
        int i = 0;
        i < iterations;
        i++
    ) {
        LuneChunk chunk;
        lune_chunk_init(&chunk);

        if (!lune_compile(
            ast,
            source,
            &chunk,
            diagnostic,
            &diagnostics
        )) {
            lune_chunk_free(&chunk);
            lune_ast_free(ast);
            free(source);
            return false;
        }

        lune_chunk_free(&chunk);
    }

    double elapsed =
        seconds_now() - start;

    printf(
        "%-24s %10.3f ms/run\n",
        "bytecode compile",
        elapsed > 0.0
            ? elapsed * 1000.0 /
                (double)iterations
            : 0.0
    );

    lune_ast_free(ast);
    free(source);

    return diagnostics.count == 0;
}

static bool benchmark_vm(
    const char *name,
    const char *source,
    int iterations,
    int64_t expected
) {
    Diagnostics diagnostics = {0};
    LuneAst *ast = NULL;
    LuneChunk chunk;

    if (!compile_source(
        source,
        strlen(source),
        &ast,
        &chunk,
        &diagnostics
    )) {
        return false;
    }

    LuneVM *vm =
        lune_vm_new(
            diagnostic,
            &diagnostics
        );

    if (vm == NULL) {
        lune_chunk_free(&chunk);
        lune_ast_free(ast);
        return false;
    }

    LuneValue result =
        lune_value_null();

    /*
     * One untimed warmup run keeps benchmark
     * output focused on steady-state VM work.
     */
    if (!lune_vm_run(
        vm,
        &chunk,
        &result
    ) || result.kind != LUNE_VALUE_INT || result.as.integer != expected) {
        lune_vm_free(vm);
        lune_chunk_free(&chunk);
        lune_ast_free(ast);
        return false;
    }

    double start = seconds_now();

    for (
        int i = 0;
        i < iterations;
        i++
    ) {
        if (!lune_vm_run(
            vm,
            &chunk,
            &result
        ) || result.kind != LUNE_VALUE_INT || result.as.integer != expected) {
            lune_vm_free(vm);
            lune_chunk_free(&chunk);
            lune_ast_free(ast);
            return false;
        }
    }

    double elapsed =
        seconds_now() - start;

    printf(
        "%-24s %10.3f ms/run\n",
        name,
        elapsed > 0.0
            ? elapsed * 1000.0 /
                (double)iterations
            : 0.0
    );

    lune_vm_free(vm);
    lune_chunk_free(&chunk);
    lune_ast_free(ast);

    return diagnostics.count == 0;
}

int main(void) {
    bool ok = true;

    ok = benchmark_frontend() && ok;
    ok = benchmark_compile() && ok;

    ok = benchmark_vm(
        "integer loop",
        "i := 0\n"
        "x := 0\n"
        "while i < 500000 {\n"
        "  x = x + 1\n"
        "  i = i + 1\n"
        "}\n"
        "x\n",
        5,
        500000
    ) && ok;

    ok = benchmark_vm(
        "function calls",
        "inc := fn(x) => x + 1\n"
        "i := 0\n"
        "x := 0\n"
        "while i < 50000 {\n"
        "  x = inc(x)\n"
        "  i = i + 1\n"
        "}\n"
        "x\n",
        5,
        50000
    ) && ok;

    ok = benchmark_vm(
        "list indexing",
        "items := [1,2,3,4,5,6,7,8]\n"
        "i := 0\n"
        "sum := 0\n"
        "while i < 200000 {\n"
        "  sum = sum + items[i % 8]\n"
        "  i = i + 1\n"
        "}\n"
        "sum\n",
        5,
        900000
    ) && ok;

    ok = benchmark_vm(
        "map lookup",
        "m := {value: 7}\n"
        "i := 0\n"
        "sum := 0\n"
        "while i < 200000 {\n"
        "  sum = sum + m.value\n"
        "  i = i + 1\n"
        "}\n"
        "sum\n",
        5,
        1400000
    ) && ok;

    ok = benchmark_vm(
        "string workload",
        "i := 0\n"
        "s := \"\"\n"
        "while i < 2000 {\n"
        "  s = s + \"x\"\n"
        "  i = i + 1\n"
        "}\n"
        "i\n",
        3,
        2000
    ) && ok;

    ok = benchmark_vm(
        "GC-heavy allocation",
        "i := 0\n"
        "last := null\n"
        "while i < 20000 {\n"
        "  last = {\n"
        "    value: i,\n"
        "    pair: [i, i + 1],\n"
        "    tag: \"x\",\n"
        "  }\n"
        "  i = i + 1\n"
        "}\n"
        "i\n",
        3,
        20000
    ) && ok;

    ok = benchmark_vm(
        "large map lookup",
        "m := {}\n"
        "i := 0\n"
        "while i < 1024 {\n"
        "  m[str(i)] = i\n"
        "  i = i + 1\n"
        "}\n"
        "i = 0\n"
        "sum := 0\n"
        "while i < 200000 {\n"
        "  sum = sum + m[\"1023\"]\n"
        "  i = i + 1\n"
        "}\n"
        "sum\n",
        5,
        204600000
    ) && ok;

    ok = benchmark_vm(
        "local integer loop",
        "run := fn() => {\n"
        "  i := 0\n"
        "  x := 0\n"
        "  while i < 500000 {\n"
        "    x = x + 1\n"
        "    i = i + 1\n"
        "  }\n"
        "  x\n"
        "}\n"
        "run()\n",
        5,
        500000
    ) && ok;

    if (!ok) {
        fputs(
            "benchmark failed\n",
            stderr
        );
        return 1;
    }

    return 0;
}
