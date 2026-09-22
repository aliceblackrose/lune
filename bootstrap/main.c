#include "ast.h"
#include "bytecode.h"
#include "compiler.h"
#include "lexer.h"
#include "parser.h"
#include "value.h"
#include "vm.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    char *buffer = malloc((size_t)size + 1);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }
    size_t read = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (read != (size_t)size) {
        free(buffer);
        return NULL;
    }

    buffer[read] = '\0';
    *length = read;
    return buffer;
}

static void print_diagnostic(
    void *context,
    LuneSpan span,
    const char *message
) {
    fprintf(
        stderr,
        "%s:%zu:%zu: error: %s\n",
        (const char *)context,
        span.line,
        span.column,
        message
    );
}

static int lex_file(
    const char *path,
    const char *source,
    size_t length
) {
    LuneLexer lexer;
    lune_lexer_init(
        &lexer, source, length, print_diagnostic, (void *)path
    );

    for (;;) {
        LuneToken token = lune_lexer_next(&lexer);
        printf(
            "%zu:%zu %-12s",
            token.span.line,
            token.span.column,
            lune_token_kind_name(token.kind)
        );
        if (
            token.span.length > 0 &&
            token.kind != LUNE_TOKEN_NEWLINE
        ) {
            printf(
                " %.*s",
                (int)token.span.length,
                source + token.span.offset
            );
        }
        putchar('\n');
        if (token.kind == LUNE_TOKEN_EOF) break;
    }
    return lexer.had_error ? 1 : 0;
}

static LuneAst *parse_source(
    const char *path,
    const char *source,
    size_t length,
    bool *ok
) {
    LuneParser parser;
    lune_parser_init(
        &parser,
        source,
        length,
        print_diagnostic,
        (void *)path
    );
    LuneAst *ast = lune_parse_program(&parser);
    *ok = ast != NULL && !parser.had_error;
    return ast;
}

static int parse_file(
    const char *path,
    const char *source,
    size_t length,
    bool dump
) {
    bool ok = false;
    LuneAst *ast = parse_source(path, source, length, &ok);
    if (ok && dump) lune_ast_dump(stdout, ast, source);
    lune_ast_free(ast);
    return ok ? 0 : 1;
}

static int execute_file(
    const char *path,
    const char *source,
    size_t length,
    bool print_result,
    int process_argc,
    const char *const *process_argv
) {
    bool ok = false;
    LuneAst *ast = parse_source(path, source, length, &ok);
    if (!ok) {
        lune_ast_free(ast);
        return 1;
    }

    LuneChunk chunk;
    lune_chunk_init(&chunk);
    ok = lune_compile(
        ast,
        source,
        &chunk,
        print_diagnostic,
        (void *)path
    );

    LuneVM *vm = NULL;
    LuneValue result = lune_value_null();
    if (ok) {
        vm = lune_vm_new(print_diagnostic, (void *)path);
        if (vm == NULL) {
            fprintf(stderr, "%s: error: out of memory\n", path);
            ok = false;
        } else {
            lune_vm_set_process_args(
                vm,
                process_argc,
                process_argv
            );
            lune_vm_set_script_path(
                vm, path
            );
            ok = lune_vm_run(
                vm, &chunk, &result
            );
        }
    }

    int exit_status = 0;
    bool requested_exit =
        vm != NULL &&
        lune_vm_exit_status(
            vm, &exit_status
        );

    if (
        ok &&
        print_result &&
        !requested_exit
    ) {
        lune_value_print(stdout, result);
        putchar('\n');
    }

    lune_vm_free(vm);
    lune_chunk_free(&chunk);
    lune_ast_free(ast);

    if (!ok) return 1;
    return requested_exit
        ? exit_status
        : 0;
}

static void usage(const char *program) {
    fprintf(
        stderr,
        "usage: %s <lex|check|parse|run|eval> FILE [ARGS...]\n",
        program
    );
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }

    bool runtime_command =
        strcmp(argv[1], "run") == 0 ||
        strcmp(argv[1], "eval") == 0;

    if (!runtime_command && argc != 3) {
        usage(argv[0]);
        return 2;
    }

    size_t length = 0;
    char *source = read_file(argv[2], &length);
    if (source == NULL) {
        fprintf(
            stderr,
            "lune-bootstrap: unable to read %s\n",
            argv[2]
        );
        return 1;
    }

    int result;
    if (strcmp(argv[1], "lex") == 0) {
        result = lex_file(argv[2], source, length);
    } else if (strcmp(argv[1], "check") == 0) {
        result = parse_file(argv[2], source, length, false);
    } else if (strcmp(argv[1], "parse") == 0) {
        result = parse_file(argv[2], source, length, true);
    } else if (strcmp(argv[1], "run") == 0) {
        result = execute_file(
            argv[2],
            source,
            length,
            false,
            argc - 3,
            (const char *const *)(argv + 3)
        );
    } else if (strcmp(argv[1], "eval") == 0) {
        result = execute_file(
            argv[2],
            source,
            length,
            true,
            argc - 3,
            (const char *const *)(argv + 3)
        );
    } else {
        usage(argv[0]);
        result = 2;
    }

    free(source);
    return result;
}
