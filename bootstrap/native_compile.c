#include "native_compile.h"

#include "ast.h"
#include "compiler.h"
#include "parser.h"
#include "platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *wrap_module_source(
    const char *source,
    size_t source_length,
    size_t *wrapped_length,
    char *error,
    size_t error_capacity
) {
    static const char prefix[] =
        "fn() => {\n";
    static const char suffix[] =
        "\n}\n";

    if (
        source_length >
        SIZE_MAX -
            (sizeof(prefix) - 1) -
            (sizeof(suffix) - 1) -
            1
    ) {
        if (
            error != NULL &&
            error_capacity > 0
        ) {
            (void)snprintf(
                error,
                error_capacity,
                "module source is too large"
            );
        }
        return NULL;
    }

    size_t length =
        (sizeof(prefix) - 1) +
        source_length +
        (sizeof(suffix) - 1);

    char *wrapped =
        malloc(length + 1);

    if (wrapped == NULL) {
        if (
            error != NULL &&
            error_capacity > 0
        ) {
            (void)snprintf(
                error,
                error_capacity,
                "out of memory"
            );
        }
        return NULL;
    }

    size_t offset = 0;

    memcpy(
        wrapped + offset,
        prefix,
        sizeof(prefix) - 1
    );
    offset += sizeof(prefix) - 1;

    memcpy(
        wrapped + offset,
        source,
        source_length
    );
    offset += source_length;

    memcpy(
        wrapped + offset,
        suffix,
        sizeof(suffix) - 1
    );
    offset += sizeof(suffix) - 1;

    wrapped[offset] = '\0';
    *wrapped_length = length;
    return wrapped;
}

bool lune_native_compile_file(
    void *context,
    const char *path,
    bool module,
    LuneChunk *chunk,
    char *error,
    size_t error_capacity
) {
    LuneNativeCompiler *compiler =
        context;

    if (
        error != NULL &&
        error_capacity > 0
    ) {
        error[0] = '\0';
    }

    char *source = NULL;
    size_t source_length = 0;

    if (!lune_platform_read_file(
        path,
        &source,
        &source_length,
        error,
        error_capacity
    )) {
        return false;
    }

    char *input = source;
    size_t input_length =
        source_length;

    if (module) {
        input = wrap_module_source(
            source,
            source_length,
            &input_length,
            error,
            error_capacity
        );

        if (input == NULL) {
            free(source);
            return false;
        }
    }

    LuneParser parser;
    lune_parser_init(
        &parser,
        input,
        input_length,
        compiler != NULL
            ? compiler->diagnostic
            : NULL,
        compiler != NULL
            ? compiler->diagnostic_context
            : NULL
    );

    LuneAst *ast =
        lune_parse_program(&parser);

    bool ok =
        ast != NULL &&
        !parser.had_error;

    if (ok) {
        ok = lune_compile(
            ast,
            input,
            chunk,
            compiler != NULL
                ? compiler->diagnostic
                : NULL,
            compiler != NULL
                ? compiler->diagnostic_context
                : NULL
        );
    }

    lune_ast_free(ast);

    if (module) {
        free(input);
    }

    free(source);
    return ok;
}
