#include "lexer.h"

#include <string.h>

static bool at_end(const LuneLexer *lexer) {
    return lexer->offset >= lexer->length;
}

static char peek(const LuneLexer *lexer) {
    return at_end(lexer) ? '\0' : lexer->source[lexer->offset];
}

static char peek_next(const LuneLexer *lexer) {
    return lexer->offset + 1 >= lexer->length ? '\0' : lexer->source[lexer->offset + 1];
}

static char advance(LuneLexer *lexer) {
    char c = lexer->source[lexer->offset++];
    if (c == '\n') {
        lexer->line++;
        lexer->column = 1;
    } else {
        lexer->column++;
    }
    return c;
}

static bool match(LuneLexer *lexer, char expected) {
    if (at_end(lexer) || peek(lexer) != expected) {
        return false;
    }
    advance(lexer);
    return true;
}

static LuneToken token_from(
    LuneTokenKind kind,
    size_t start,
    size_t line,
    size_t column,
    const LuneLexer *lexer
) {
    LuneToken token = {
        .kind = kind,
        .span = {
            .offset = start,
            .length = lexer->offset - start,
            .line = line,
            .column = column,
        },
    };
    return token;
}

static void report(LuneLexer *lexer, LuneSpan span, const char *message) {
    lexer->had_error = true;
    if (lexer->diagnostic != NULL) {
        lexer->diagnostic(lexer->diagnostic_context, span, message);
    }
}

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_hex(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static LuneTokenKind keyword_kind(const char *text, size_t length) {
#define KEYWORD(word, kind) \
    if (length == sizeof(word) - 1 && memcmp(text, word, sizeof(word) - 1) == 0) return kind
    KEYWORD("fn", LUNE_TOKEN_FN);
    KEYWORD("if", LUNE_TOKEN_IF);
    KEYWORD("else", LUNE_TOKEN_ELSE);
    KEYWORD("while", LUNE_TOKEN_WHILE);
    KEYWORD("true", LUNE_TOKEN_TRUE);
    KEYWORD("false", LUNE_TOKEN_FALSE);
    KEYWORD("null", LUNE_TOKEN_NULL);
    KEYWORD("and", LUNE_TOKEN_AND);
    KEYWORD("or", LUNE_TOKEN_OR);
    KEYWORD("not", LUNE_TOKEN_NOT);
#undef KEYWORD
    return LUNE_TOKEN_IDENTIFIER;
}

static void skip_ignored(LuneLexer *lexer) {
    for (;;) {
        while (!at_end(lexer)) {
            char c = peek(lexer);
            if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
                advance(lexer);
            } else {
                break;
            }
        }

        if (peek(lexer) == '/' && peek_next(lexer) == '/') {
            while (!at_end(lexer) && peek(lexer) != '\n') {
                advance(lexer);
            }
            continue;
        }

        break;
    }
}

static LuneToken lex_identifier(
    LuneLexer *lexer,
    size_t start,
    size_t line,
    size_t column
) {
    while (is_alpha(peek(lexer)) || is_digit(peek(lexer))) {
        advance(lexer);
    }
    LuneTokenKind kind = keyword_kind(lexer->source + start, lexer->offset - start);
    return token_from(kind, start, line, column, lexer);
}

static LuneToken lex_number(
    LuneLexer *lexer,
    size_t start,
    size_t line,
    size_t column
) {
    while (is_digit(peek(lexer))) {
        advance(lexer);
    }

    if (peek(lexer) == '.' && is_digit(peek_next(lexer))) {
        advance(lexer);
        while (is_digit(peek(lexer))) {
            advance(lexer);
        }
    }

    return token_from(LUNE_TOKEN_NUMBER, start, line, column, lexer);
}

static bool lex_unicode_escape(LuneLexer *lexer) {
    if (!match(lexer, '{')) {
        return false;
    }

    size_t digits = 0;
    while (is_hex(peek(lexer))) {
        advance(lexer);
        digits++;
    }

    return digits != 0 && match(lexer, '}');
}

static LuneToken lex_string(
    LuneLexer *lexer,
    size_t start,
    size_t line,
    size_t column
) {
    bool valid = true;

    while (!at_end(lexer) && peek(lexer) != '"' &&
           peek(lexer) != '\n' && peek(lexer) != '\r') {
        if (peek(lexer) != '\\') {
            advance(lexer);
            continue;
        }

        advance(lexer);
        if (at_end(lexer)) {
            break;
        }

        char escape = advance(lexer);
        if (escape == '"' || escape == '\\' || escape == 'n' ||
            escape == 'r' || escape == 't' || escape == '0') {
            continue;
        }

        if (escape == 'u' && lex_unicode_escape(lexer)) {
            continue;
        }

        valid = false;
        LuneSpan span = {
            .offset = lexer->offset - 1,
            .length = 1,
            .line = lexer->line,
            .column = lexer->column - 1,
        };
        report(lexer, span, "invalid string escape");
    }

    if (at_end(lexer) || peek(lexer) != '"') {
        LuneSpan span = {
            .offset = start,
            .length = lexer->offset - start,
            .line = line,
            .column = column,
        };
        report(lexer, span, "unterminated string");
        return token_from(LUNE_TOKEN_ERROR, start, line, column, lexer);
    }

    advance(lexer);
    return token_from(
        valid ? LUNE_TOKEN_STRING : LUNE_TOKEN_ERROR,
        start,
        line,
        column,
        lexer
    );
}

void lune_lexer_init(
    LuneLexer *lexer,
    const char *source,
    size_t length,
    LuneDiagnosticFn diagnostic,
    void *diagnostic_context
) {
    *lexer = (LuneLexer) {
        .source = source,
        .length = length,
        .offset = 0,
        .line = 1,
        .column = 1,
        .had_error = false,
        .diagnostic = diagnostic,
        .diagnostic_context = diagnostic_context,
    };
}

LuneToken lune_lexer_next(LuneLexer *lexer) {
    skip_ignored(lexer);

    size_t start = lexer->offset;
    size_t line = lexer->line;
    size_t column = lexer->column;

    if (at_end(lexer)) {
        return token_from(LUNE_TOKEN_EOF, start, line, column, lexer);
    }

    char c = advance(lexer);

    if (is_alpha(c)) {
        return lex_identifier(lexer, start, line, column);
    }
    if (is_digit(c)) {
        return lex_number(lexer, start, line, column);
    }

    switch (c) {
        case '\n':
            return token_from(LUNE_TOKEN_NEWLINE, start, line, column, lexer);
        case '\r':
            if (match(lexer, '\n')) {
                return token_from(LUNE_TOKEN_NEWLINE, start, line, column, lexer);
            }
            break;
        case '"':
            return lex_string(lexer, start, line, column);
        case ':':
            if (match(lexer, '=')) return token_from(LUNE_TOKEN_DECLARE, start, line, column, lexer);
            return token_from(LUNE_TOKEN_COLON, start, line, column, lexer);
        case '=':
            if (match(lexer, '>')) return token_from(LUNE_TOKEN_ARROW, start, line, column, lexer);
            if (match(lexer, '=')) return token_from(LUNE_TOKEN_EQ, start, line, column, lexer);
            return token_from(LUNE_TOKEN_ASSIGN, start, line, column, lexer);
        case '!':
            if (match(lexer, '=')) return token_from(LUNE_TOKEN_NE, start, line, column, lexer);
            break;
        case '<':
            if (match(lexer, '=')) return token_from(LUNE_TOKEN_LE, start, line, column, lexer);
            return token_from(LUNE_TOKEN_LT, start, line, column, lexer);
        case '>':
            if (match(lexer, '=')) return token_from(LUNE_TOKEN_GE, start, line, column, lexer);
            return token_from(LUNE_TOKEN_GT, start, line, column, lexer);
        case '+': return token_from(LUNE_TOKEN_PLUS, start, line, column, lexer);
        case '-': return token_from(LUNE_TOKEN_MINUS, start, line, column, lexer);
        case '*': return token_from(LUNE_TOKEN_STAR, start, line, column, lexer);
        case '/': return token_from(LUNE_TOKEN_SLASH, start, line, column, lexer);
        case '%': return token_from(LUNE_TOKEN_PERCENT, start, line, column, lexer);
        case '(': return token_from(LUNE_TOKEN_LPAREN, start, line, column, lexer);
        case ')': return token_from(LUNE_TOKEN_RPAREN, start, line, column, lexer);
        case '{': return token_from(LUNE_TOKEN_LBRACE, start, line, column, lexer);
        case '}': return token_from(LUNE_TOKEN_RBRACE, start, line, column, lexer);
        case '[': return token_from(LUNE_TOKEN_LBRACK, start, line, column, lexer);
        case ']': return token_from(LUNE_TOKEN_RBRACK, start, line, column, lexer);
        case ',': return token_from(LUNE_TOKEN_COMMA, start, line, column, lexer);
        case '.': return token_from(LUNE_TOKEN_DOT, start, line, column, lexer);
        default: break;
    }

    LuneSpan span = {
        .offset = start,
        .length = lexer->offset - start,
        .line = line,
        .column = column,
    };
    report(lexer, span, "unexpected character");
    return token_from(LUNE_TOKEN_ERROR, start, line, column, lexer);
}

const char *lune_token_kind_name(LuneTokenKind kind) {
    switch (kind) {
#define TOKEN_NAME(value) case LUNE_TOKEN_##value: return #value
        TOKEN_NAME(ERROR);
        TOKEN_NAME(EOF);
        TOKEN_NAME(NEWLINE);
        TOKEN_NAME(IDENTIFIER);
        TOKEN_NAME(NUMBER);
        TOKEN_NAME(STRING);
        TOKEN_NAME(FN);
        TOKEN_NAME(IF);
        TOKEN_NAME(ELSE);
        TOKEN_NAME(WHILE);
        TOKEN_NAME(TRUE);
        TOKEN_NAME(FALSE);
        TOKEN_NAME(NULL);
        TOKEN_NAME(AND);
        TOKEN_NAME(OR);
        TOKEN_NAME(NOT);
        TOKEN_NAME(DECLARE);
        TOKEN_NAME(ARROW);
        TOKEN_NAME(EQ);
        TOKEN_NAME(NE);
        TOKEN_NAME(LE);
        TOKEN_NAME(GE);
        TOKEN_NAME(ASSIGN);
        TOKEN_NAME(LT);
        TOKEN_NAME(GT);
        TOKEN_NAME(PLUS);
        TOKEN_NAME(MINUS);
        TOKEN_NAME(STAR);
        TOKEN_NAME(SLASH);
        TOKEN_NAME(PERCENT);
        TOKEN_NAME(LPAREN);
        TOKEN_NAME(RPAREN);
        TOKEN_NAME(LBRACE);
        TOKEN_NAME(RBRACE);
        TOKEN_NAME(LBRACK);
        TOKEN_NAME(RBRACK);
        TOKEN_NAME(COMMA);
        TOKEN_NAME(DOT);
        TOKEN_NAME(COLON);
#undef TOKEN_NAME
    }
    return "UNKNOWN";
}
