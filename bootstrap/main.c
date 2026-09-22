#include "ast.h"
#include "lexer.h"
#include "parser.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }

    char *buffer = malloc((size_t)size + 1);
    if (buffer == NULL) { fclose(file); return NULL; }
    size_t read = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (read != (size_t)size) { free(buffer); return NULL; }

    buffer[read] = '\0';
    *length = read;
    return buffer;
}

static void print_diagnostic(void *context, LuneSpan span, const char *message) {
    const char *path = context;
    fprintf(stderr, "%s:%zu:%zu: error: %s\n", path, span.line, span.column, message);
}

static int lex_file(const char *path, const char *source, size_t length) {
    LuneLexer lexer;
    lune_lexer_init(&lexer, source, length, print_diagnostic, (void *)path);

    for (;;) {
        LuneToken token = lune_lexer_next(&lexer);
        printf("%zu:%zu %-12s", token.span.line, token.span.column, lune_token_kind_name(token.kind));
        if (token.span.length > 0 && token.kind != LUNE_TOKEN_NEWLINE)
            printf(" %.*s", (int)token.span.length, source + token.span.offset);
        putchar('\n');
        if (token.kind == LUNE_TOKEN_EOF) break;
    }
    return lexer.had_error ? 1 : 0;
}

static int parse_file(const char *path, const char *source, size_t length, bool dump) {
    LuneParser parser;
    lune_parser_init(&parser, source, length, print_diagnostic, (void *)path);
    LuneAst *ast = lune_parse_program(&parser);
    int result = parser.had_error || ast == NULL ? 1 : 0;
    if (result == 0 && dump) lune_ast_dump(stdout, ast, source);
    lune_ast_free(ast);
    return result;
}

static void usage(const char *program) {
    fprintf(stderr, "usage: %s <lex|check|parse> FILE\n", program);
}

int main(int argc, char **argv) {
    if (argc != 3) { usage(argv[0]); return 2; }

    size_t length = 0;
    char *source = read_file(argv[2], &length);
    if (source == NULL) {
        fprintf(stderr, "lune-bootstrap: unable to read %s\n", argv[2]);
        return 1;
    }

    int result;
    if (strcmp(argv[1], "lex") == 0)
        result = lex_file(argv[2], source, length);
    else if (strcmp(argv[1], "check") == 0)
        result = parse_file(argv[2], source, length, false);
    else if (strcmp(argv[1], "parse") == 0)
        result = parse_file(argv[2], source, length, true);
    else {
        usage(argv[0]);
        result = 2;
    }

    free(source);
    return result;
}
