#ifndef LUNE_PARSER_H
#define LUNE_PARSER_H

#include "ast.h"
#include "lexer.h"

typedef struct {
    LuneLexer lexer;
    LuneToken current;
    LuneToken next;
    bool had_error;
    bool oom;
    LuneDiagnosticFn diagnostic;
    void *diagnostic_context;
} LuneParser;

void lune_parser_init(
    LuneParser *parser,
    const char *source,
    size_t length,
    LuneDiagnosticFn diagnostic,
    void *diagnostic_context
);

LuneAst *lune_parse_program(LuneParser *parser);

#endif
