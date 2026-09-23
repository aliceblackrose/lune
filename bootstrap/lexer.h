#ifndef LUNE_LEXER_H
#define LUNE_LEXER_H

#include "source.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    LUNE_TOKEN_ERROR = 0,
    LUNE_TOKEN_EOF,
    LUNE_TOKEN_NEWLINE,

    LUNE_TOKEN_IDENTIFIER,
    LUNE_TOKEN_NUMBER,
    LUNE_TOKEN_STRING,

    LUNE_TOKEN_FN,
    LUNE_TOKEN_IF,
    LUNE_TOKEN_ELSE,
    LUNE_TOKEN_WHILE,
    LUNE_TOKEN_TRUE,
    LUNE_TOKEN_FALSE,
    LUNE_TOKEN_NULL,
    LUNE_TOKEN_AND,
    LUNE_TOKEN_OR,
    LUNE_TOKEN_NOT,

    LUNE_TOKEN_DECLARE,
    LUNE_TOKEN_ARROW,
    LUNE_TOKEN_EQ,
    LUNE_TOKEN_NE,
    LUNE_TOKEN_LE,
    LUNE_TOKEN_GE,
    LUNE_TOKEN_ASSIGN,
    LUNE_TOKEN_LT,
    LUNE_TOKEN_GT,
    LUNE_TOKEN_PLUS,
    LUNE_TOKEN_MINUS,
    LUNE_TOKEN_STAR,
    LUNE_TOKEN_SLASH,
    LUNE_TOKEN_PERCENT,

    LUNE_TOKEN_LPAREN,
    LUNE_TOKEN_RPAREN,
    LUNE_TOKEN_LBRACE,
    LUNE_TOKEN_RBRACE,
    LUNE_TOKEN_LBRACK,
    LUNE_TOKEN_RBRACK,
    LUNE_TOKEN_COMMA,
    LUNE_TOKEN_DOT,
    LUNE_TOKEN_COLON
} LuneTokenKind;

typedef struct {
    LuneTokenKind kind;
    LuneSpan span;
} LuneToken;

typedef struct {
    const char *source;
    size_t length;
    size_t offset;
    size_t line;
    size_t column;
    bool had_error;
    LuneDiagnosticFn diagnostic;
    void *diagnostic_context;
} LuneLexer;

void lune_lexer_init(
    LuneLexer *lexer,
    const char *source,
    size_t length,
    LuneDiagnosticFn diagnostic,
    void *diagnostic_context
);

LuneToken lune_lexer_next(LuneLexer *lexer);
const char *lune_token_kind_name(LuneTokenKind kind);

#endif
