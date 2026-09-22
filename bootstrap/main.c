#include "lexer.h"

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

static void print_diagnostic(void *context, LuneSpan span, const char *message) {
    const char *path = context;
    fprintf(stderr, "%s:%zu:%zu: error: %s\n", path, span.line, span.column, message);
}

static int lex_file(const char *path) {
    size_t length = 0;
    char *source = read_file(path, &length);
    if (source == NULL) {
        fprintf(stderr, "lune-bootstrap: unable to read %s\n", path);
        return 1;
    }

    LuneLexer lexer;
    lune_lexer_init(&lexer, source, length, print_diagnostic, (void *)path);

    for (;;) {
        LuneToken token = lune_lexer_next(&lexer);
        printf("%zu:%zu %-12s", token.span.line, token.span.column, lune_token_kind_name(token.kind));
        if (token.span.length > 0 && token.kind != LUNE_TOKEN_NEWLINE) {
            printf(" %.*s", (int)token.span.length, source + token.span.offset);
        }
        putchar('\n');
        if (token.kind == LUNE_TOKEN_EOF) break;
    }

    int result = lexer.had_error ? 1 : 0;
    free(source);
    return result;
}

static void usage(const char *program) {
    fprintf(stderr, "usage: %s lex FILE\n", program);
}

int main(int argc, char **argv) {
    if (argc != 3 || strcmp(argv[1], "lex") != 0) {
        usage(argv[0]);
        return 2;
    }
    return lex_file(argv[2]);
}
