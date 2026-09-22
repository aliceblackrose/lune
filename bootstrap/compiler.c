#include "compiler.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    LuneSpan name;
    int depth;
    uint16_t slot;
} Local;

typedef struct {
    const char *source;
    LuneChunk *chunk;
    LuneDiagnosticFn diagnostic;
    void *diagnostic_context;
    bool had_error;
    bool oom;

    int scope_depth;
    Local locals[UINT8_MAX + 1];
    size_t local_count;

    uint16_t *declared_globals;
    size_t declared_global_count;
    size_t declared_global_capacity;
} Compiler;

static void error_at(Compiler *compiler, LuneSpan span, const char *message) {
    compiler->had_error = true;
    if (compiler->diagnostic != NULL) {
        compiler->diagnostic(compiler->diagnostic_context, span, message);
    }
}

static bool emit_byte(Compiler *compiler, uint8_t byte, LuneSpan span) {
    if (lune_chunk_write(compiler->chunk, byte, span)) return true;
    if (!compiler->oom) {
        compiler->oom = true;
        error_at(compiler, span, "out of memory");
    }
    return false;
}

static bool emit_op(Compiler *compiler, LuneOpcode opcode, LuneSpan span) {
    return emit_byte(compiler, (uint8_t)opcode, span);
}

static bool emit_u16(Compiler *compiler, uint16_t value, LuneSpan span) {
    return emit_byte(compiler, (uint8_t)(value >> 8), span) &&
        emit_byte(compiler, (uint8_t)(value & 0xff), span);
}

static bool emit_indexed(
    Compiler *compiler,
    LuneOpcode opcode,
    uint16_t index,
    LuneSpan span
) {
    return emit_op(compiler, opcode, span) &&
        emit_u16(compiler, index, span);
}

static size_t emit_jump(
    Compiler *compiler,
    LuneOpcode opcode,
    LuneSpan span
) {
    if (!emit_op(compiler, opcode, span)) return SIZE_MAX;
    size_t operand = compiler->chunk->count;
    if (!emit_u16(compiler, UINT16_MAX, span)) return SIZE_MAX;
    return operand;
}

static bool patch_jump(Compiler *compiler, size_t operand, LuneSpan span) {
    if (operand == SIZE_MAX || compiler->chunk->count < operand + 2) {
        return false;
    }

    size_t jump = compiler->chunk->count - operand - 2;
    if (jump > UINT16_MAX) {
        error_at(compiler, span, "jump is too large");
        return false;
    }

    compiler->chunk->code[operand] = (uint8_t)(jump >> 8);
    compiler->chunk->code[operand + 1] = (uint8_t)(jump & 0xff);
    return true;
}

static bool emit_loop(Compiler *compiler, size_t start, LuneSpan span) {
    if (!emit_op(compiler, LUNE_OP_LOOP, span)) return false;

    size_t distance = (compiler->chunk->count + 2) - start;
    if (distance > UINT16_MAX) {
        error_at(compiler, span, "loop body is too large");
        return false;
    }
    return emit_u16(compiler, (uint16_t)distance, span);
}

static bool span_equal(const Compiler *compiler, LuneSpan a, LuneSpan b) {
    return a.length == b.length &&
        memcmp(
            compiler->source + a.offset,
            compiler->source + b.offset,
            a.length
        ) == 0;
}

static int resolve_local(const Compiler *compiler, LuneSpan name) {
    for (size_t i = compiler->local_count; i > 0; i--) {
        if (span_equal(compiler, compiler->locals[i - 1].name, name)) {
            return (int)compiler->locals[i - 1].slot;
        }
    }
    return -1;
}

static bool local_declared_here(const Compiler *compiler, LuneSpan name) {
    for (size_t i = compiler->local_count; i > 0; i--) {
        const Local *local = &compiler->locals[i - 1];
        if (local->depth < compiler->scope_depth) break;
        if (span_equal(compiler, local->name, name)) return true;
    }
    return false;
}

static bool add_local(
    Compiler *compiler,
    LuneSpan name,
    uint16_t *slot
) {
    if (compiler->local_count >= UINT8_MAX + 1u) {
        error_at(compiler, name, "too many local bindings in scope");
        return false;
    }

    *slot = (uint16_t)compiler->local_count;
    compiler->locals[compiler->local_count++] = (Local){
        .name = name,
        .depth = compiler->scope_depth,
        .slot = *slot,
    };
    return true;
}

static void begin_scope(Compiler *compiler) {
    compiler->scope_depth++;
}

static void end_scope(Compiler *compiler) {
    compiler->scope_depth--;
    while (
        compiler->local_count > 0 &&
        compiler->locals[compiler->local_count - 1].depth >
            compiler->scope_depth
    ) {
        compiler->local_count--;
    }
}

static bool intern_bytes(
    Compiler *compiler,
    const char *chars,
    size_t length,
    LuneSpan span,
    uint16_t *index
) {
    if (lune_chunk_intern_name(
        compiler->chunk, chars, length, index
    )) return true;

    error_at(compiler, span, "too many strings/names or out of memory");
    return false;
}

static bool intern_span(
    Compiler *compiler,
    LuneSpan span,
    uint16_t *index
) {
    return intern_bytes(
        compiler,
        compiler->source + span.offset,
        span.length,
        span,
        index
    );
}

static bool global_declared(const Compiler *compiler, uint16_t name) {
    for (size_t i = 0; i < compiler->declared_global_count; i++) {
        if (compiler->declared_globals[i] == name) return true;
    }
    return false;
}

static bool declare_global(
    Compiler *compiler,
    uint16_t name,
    LuneSpan span
) {
    if (global_declared(compiler, name)) {
        error_at(compiler, span, "binding already declared in this scope");
        return false;
    }

    if (
        compiler->declared_global_count ==
        compiler->declared_global_capacity
    ) {
        size_t next = compiler->declared_global_capacity == 0
            ? 8
            : compiler->declared_global_capacity * 2;
        uint16_t *grown = realloc(
            compiler->declared_globals,
            next * sizeof(*grown)
        );
        if (grown == NULL) {
            error_at(compiler, span, "out of memory");
            return false;
        }
        compiler->declared_globals = grown;
        compiler->declared_global_capacity = next;
    }

    compiler->declared_globals[compiler->declared_global_count++] = name;
    return true;
}

static bool encode_utf8(uint32_t codepoint, char *out, size_t *count) {
    if (codepoint <= 0x7f) {
        out[0] = (char)codepoint;
        *count = 1;
        return true;
    }
    if (codepoint <= 0x7ff) {
        out[0] = (char)(0xc0u | (codepoint >> 6));
        out[1] = (char)(0x80u | (codepoint & 0x3fu));
        *count = 2;
        return true;
    }
    if (
        codepoint >= 0xd800u &&
        codepoint <= 0xdfffu
    ) {
        return false;
    }
    if (codepoint <= 0xffffu) {
        out[0] = (char)(0xe0u | (codepoint >> 12));
        out[1] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
        out[2] = (char)(0x80u | (codepoint & 0x3fu));
        *count = 3;
        return true;
    }
    if (codepoint <= 0x10ffffu) {
        out[0] = (char)(0xf0u | (codepoint >> 18));
        out[1] = (char)(0x80u | ((codepoint >> 12) & 0x3fu));
        out[2] = (char)(0x80u | ((codepoint >> 6) & 0x3fu));
        out[3] = (char)(0x80u | (codepoint & 0x3fu));
        *count = 4;
        return true;
    }
    return false;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static bool decode_string(
    Compiler *compiler,
    LuneSpan span,
    char **chars_out,
    size_t *length_out
) {
    if (
        span.length < 2 ||
        compiler->source[span.offset] != '"' ||
        compiler->source[span.offset + span.length - 1] != '"'
    ) {
        error_at(compiler, span, "invalid string token");
        return false;
    }

    char *decoded = malloc(span.length);
    if (decoded == NULL) {
        error_at(compiler, span, "out of memory");
        return false;
    }

    size_t write = 0;
    size_t i = span.offset + 1;
    size_t end = span.offset + span.length - 1;

    while (i < end) {
        char c = compiler->source[i++];
        if (c != '\\') {
            decoded[write++] = c;
            continue;
        }

        if (i >= end) {
            free(decoded);
            error_at(compiler, span, "invalid string escape");
            return false;
        }

        char escape = compiler->source[i++];
        switch (escape) {
            case '"': decoded[write++] = '"'; break;
            case '\\': decoded[write++] = '\\'; break;
            case 'n': decoded[write++] = '\n'; break;
            case 'r': decoded[write++] = '\r'; break;
            case 't': decoded[write++] = '\t'; break;
            case '0': decoded[write++] = '\0'; break;
            case 'u': {
                if (i >= end || compiler->source[i++] != '{') {
                    free(decoded);
                    error_at(compiler, span, "invalid unicode escape");
                    return false;
                }

                uint32_t codepoint = 0;
                size_t digits = 0;
                while (i < end && compiler->source[i] != '}') {
                    int value = hex_value(compiler->source[i++]);
                    if (value < 0 || codepoint > 0x10ffffu / 16u) {
                        free(decoded);
                        error_at(
                            compiler, span, "invalid unicode escape"
                        );
                        return false;
                    }
                    codepoint = codepoint * 16u + (uint32_t)value;
                    digits++;
                }

                if (
                    digits == 0 ||
                    i >= end ||
                    compiler->source[i++] != '}'
                ) {
                    free(decoded);
                    error_at(compiler, span, "invalid unicode escape");
                    return false;
                }

                char encoded[4];
                size_t encoded_count = 0;
                if (!encode_utf8(codepoint, encoded, &encoded_count)) {
                    free(decoded);
                    error_at(
                        compiler, span, "unicode code point is out of range"
                    );
                    return false;
                }
                memcpy(decoded + write, encoded, encoded_count);
                write += encoded_count;
                break;
            }
            default:
                free(decoded);
                error_at(compiler, span, "invalid string escape");
                return false;
        }
    }

    *chars_out = decoded;
    *length_out = write;
    return true;
}

static bool emit_string_span(
    Compiler *compiler,
    LuneSpan span,
    bool quoted
) {
    uint16_t index;
    if (!quoted) {
        return intern_span(compiler, span, &index) &&
            emit_indexed(compiler, LUNE_OP_STRING, index, span);
    }

    char *decoded = NULL;
    size_t length = 0;
    if (!decode_string(compiler, span, &decoded, &length)) {
        return false;
    }

    bool ok = intern_bytes(
        compiler, decoded, length, span, &index
    ) && emit_indexed(
        compiler, LUNE_OP_STRING, index, span
    );
    free(decoded);
    return ok;
}

static bool compile_node(Compiler *compiler, const LuneAst *node);

static bool compile_sequence(
    Compiler *compiler,
    const LuneAstList *statements,
    LuneSpan span
) {
    if (statements->count == 0) {
        return emit_op(compiler, LUNE_OP_NULL, span);
    }

    for (size_t i = 0; i < statements->count; i++) {
        if (!compile_node(compiler, statements->items[i])) return false;
        if (
            i + 1 < statements->count &&
            !emit_op(
                compiler,
                LUNE_OP_POP,
                statements->items[i]->span
            )
        ) return false;
    }
    return true;
}

static bool compile_number(Compiler *compiler, const LuneAst *node) {
    char *text = malloc(node->span.length + 1);
    if (text == NULL) {
        error_at(compiler, node->span, "out of memory");
        return false;
    }

    memcpy(
        text,
        compiler->source + node->span.offset,
        node->span.length
    );
    text[node->span.length] = '\0';
    errno = 0;

    LuneValue value;
    if (strchr(text, '.') != NULL) {
        char *end = NULL;
        double number = strtod(text, &end);
        if (errno == ERANGE || end == text || *end != '\0') {
            free(text);
            error_at(
                compiler, node->span, "invalid floating-point literal"
            );
            return false;
        }
        value = lune_value_float(number);
    } else {
        char *end = NULL;
        long long number = strtoll(text, &end, 10);
        if (errno == ERANGE || end == text || *end != '\0') {
            free(text);
            error_at(
                compiler, node->span, "integer literal is out of range"
            );
            return false;
        }
        value = lune_value_int((int64_t)number);
    }
    free(text);

    uint16_t index;
    if (!lune_chunk_add_constant(compiler->chunk, value, &index)) {
        error_at(
            compiler, node->span, "too many constants or out of memory"
        );
        return false;
    }
    return emit_indexed(
        compiler, LUNE_OP_CONSTANT, index, node->span
    );
}

static bool compile_name(Compiler *compiler, const LuneAst *node) {
    int local = resolve_local(compiler, node->span);
    if (local >= 0) {
        return emit_indexed(
            compiler,
            LUNE_OP_GET_LOCAL,
            (uint16_t)local,
            node->span
        );
    }

    uint16_t name;
    return intern_span(compiler, node->span, &name) &&
        emit_indexed(
            compiler, LUNE_OP_GET_GLOBAL, name, node->span
        );
}

static bool compile_binary(Compiler *compiler, const LuneAst *node) {
    LuneTokenKind op = node->as.binary.op;

    if (op == LUNE_TOKEN_AND) {
        if (!compile_node(compiler, node->as.binary.left)) return false;
        size_t end = emit_jump(
            compiler, LUNE_OP_JUMP_IF_FALSE, node->span
        );
        if (
            !emit_op(compiler, LUNE_OP_POP, node->span) ||
            !compile_node(compiler, node->as.binary.right)
        ) return false;
        return patch_jump(compiler, end, node->span);
    }

    if (op == LUNE_TOKEN_OR) {
        if (!compile_node(compiler, node->as.binary.left)) return false;
        size_t rhs = emit_jump(
            compiler, LUNE_OP_JUMP_IF_FALSE, node->span
        );
        size_t end = emit_jump(
            compiler, LUNE_OP_JUMP, node->span
        );
        if (
            !patch_jump(compiler, rhs, node->span) ||
            !emit_op(compiler, LUNE_OP_POP, node->span) ||
            !compile_node(compiler, node->as.binary.right)
        ) return false;
        return patch_jump(compiler, end, node->span);
    }

    if (
        !compile_node(compiler, node->as.binary.left) ||
        !compile_node(compiler, node->as.binary.right)
    ) return false;

    LuneOpcode opcode;
    switch (op) {
        case LUNE_TOKEN_PLUS: opcode = LUNE_OP_ADD; break;
        case LUNE_TOKEN_MINUS: opcode = LUNE_OP_SUBTRACT; break;
        case LUNE_TOKEN_STAR: opcode = LUNE_OP_MULTIPLY; break;
        case LUNE_TOKEN_SLASH: opcode = LUNE_OP_DIVIDE; break;
        case LUNE_TOKEN_PERCENT: opcode = LUNE_OP_MODULO; break;
        case LUNE_TOKEN_EQ: opcode = LUNE_OP_EQUAL; break;
        case LUNE_TOKEN_NE: opcode = LUNE_OP_NOT_EQUAL; break;
        case LUNE_TOKEN_LT: opcode = LUNE_OP_LESS; break;
        case LUNE_TOKEN_LE: opcode = LUNE_OP_LESS_EQUAL; break;
        case LUNE_TOKEN_GT: opcode = LUNE_OP_GREATER; break;
        case LUNE_TOKEN_GE: opcode = LUNE_OP_GREATER_EQUAL; break;
        default:
            error_at(
                compiler, node->span, "unsupported binary operator"
            );
            return false;
    }
    return emit_op(compiler, opcode, node->span);
}

static bool compile_if(Compiler *compiler, const LuneAst *node) {
    if (!compile_node(compiler, node->as.if_expr.condition)) {
        return false;
    }

    size_t else_jump = emit_jump(
        compiler, LUNE_OP_JUMP_IF_FALSE, node->span
    );
    if (
        !emit_op(compiler, LUNE_OP_POP, node->span) ||
        !compile_node(compiler, node->as.if_expr.then_branch)
    ) return false;

    size_t end_jump = emit_jump(
        compiler, LUNE_OP_JUMP, node->span
    );
    if (
        !patch_jump(compiler, else_jump, node->span) ||
        !emit_op(compiler, LUNE_OP_POP, node->span)
    ) return false;

    if (node->as.if_expr.else_branch != NULL) {
        if (!compile_node(
            compiler, node->as.if_expr.else_branch
        )) return false;
    } else if (!emit_op(compiler, LUNE_OP_NULL, node->span)) {
        return false;
    }

    return patch_jump(compiler, end_jump, node->span);
}

static bool compile_while(Compiler *compiler, const LuneAst *node) {
    size_t loop_start = compiler->chunk->count;
    if (!compile_node(compiler, node->as.while_stmt.condition)) {
        return false;
    }

    size_t exit_jump = emit_jump(
        compiler, LUNE_OP_JUMP_IF_FALSE, node->span
    );
    if (
        !emit_op(compiler, LUNE_OP_POP, node->span) ||
        !compile_node(compiler, node->as.while_stmt.body) ||
        !emit_op(compiler, LUNE_OP_POP, node->span) ||
        !emit_loop(compiler, loop_start, node->span)
    ) return false;

    if (
        !patch_jump(compiler, exit_jump, node->span) ||
        !emit_op(compiler, LUNE_OP_POP, node->span)
    ) return false;

    return emit_op(compiler, LUNE_OP_NULL, node->span);
}

static bool compile_declare(Compiler *compiler, const LuneAst *node) {
    LuneSpan name_span = node->as.declare.name;

    if (compiler->scope_depth > 0) {
        if (local_declared_here(compiler, name_span)) {
            error_at(
                compiler,
                name_span,
                "binding already declared in this scope"
            );
            return false;
        }

        if (!compile_node(compiler, node->as.declare.value)) {
            return false;
        }

        uint16_t slot;
        return add_local(compiler, name_span, &slot) &&
            emit_indexed(
                compiler, LUNE_OP_SET_LOCAL, slot, name_span
            );
    }

    uint16_t name;
    if (
        !intern_span(compiler, name_span, &name) ||
        !declare_global(compiler, name, name_span) ||
        !compile_node(compiler, node->as.declare.value)
    ) return false;

    return emit_indexed(
        compiler, LUNE_OP_DEFINE_GLOBAL, name, name_span
    );
}

static bool compile_list(Compiler *compiler, const LuneAst *node) {
    if (node->as.list.elements.count > UINT16_MAX) {
        error_at(compiler, node->span, "list literal is too large");
        return false;
    }

    for (size_t i = 0; i < node->as.list.elements.count; i++) {
        if (!compile_node(
            compiler, node->as.list.elements.items[i]
        )) return false;
    }

    return emit_indexed(
        compiler,
        LUNE_OP_LIST,
        (uint16_t)node->as.list.elements.count,
        node->span
    );
}

static bool compile_map(Compiler *compiler, const LuneAst *node) {
    if (node->as.map.entries.count > UINT16_MAX) {
        error_at(compiler, node->span, "map literal is too large");
        return false;
    }

    for (size_t i = 0; i < node->as.map.entries.count; i++) {
        const LuneMapEntry *entry = &node->as.map.entries.items[i];
        if (!emit_string_span(
            compiler, entry->key, entry->quoted
        )) return false;
        if (!compile_node(compiler, entry->value)) return false;
    }

    return emit_indexed(
        compiler,
        LUNE_OP_MAP,
        (uint16_t)node->as.map.entries.count,
        node->span
    );
}

static bool compile_index(Compiler *compiler, const LuneAst *node) {
    return compile_node(compiler, node->as.index.object) &&
        compile_node(compiler, node->as.index.index) &&
        emit_op(compiler, LUNE_OP_GET_INDEX, node->span);
}

static bool compile_member(Compiler *compiler, const LuneAst *node) {
    uint16_t name;
    return compile_node(compiler, node->as.member.object) &&
        intern_span(compiler, node->as.member.name, &name) &&
        emit_indexed(
            compiler, LUNE_OP_GET_FIELD, name, node->span
        );
}

static bool compile_assign(Compiler *compiler, const LuneAst *node) {
    const LuneAst *target = node->as.assign.target;

    if (target->kind == LUNE_AST_NAME) {
        if (!compile_node(compiler, node->as.assign.value)) {
            return false;
        }

        int local = resolve_local(compiler, target->span);
        if (local >= 0) {
            return emit_indexed(
                compiler,
                LUNE_OP_SET_LOCAL,
                (uint16_t)local,
                target->span
            );
        }

        uint16_t name;
        return intern_span(compiler, target->span, &name) &&
            emit_indexed(
                compiler, LUNE_OP_SET_GLOBAL, name, target->span
            );
    }

    if (target->kind == LUNE_AST_INDEX) {
        return compile_node(compiler, target->as.index.object) &&
            compile_node(compiler, target->as.index.index) &&
            compile_node(compiler, node->as.assign.value) &&
            emit_op(compiler, LUNE_OP_SET_INDEX, node->span);
    }

    if (target->kind == LUNE_AST_MEMBER) {
        uint16_t name;
        return compile_node(compiler, target->as.member.object) &&
            compile_node(compiler, node->as.assign.value) &&
            intern_span(compiler, target->as.member.name, &name) &&
            emit_indexed(
                compiler, LUNE_OP_SET_FIELD, name, node->span
            );
    }

    error_at(compiler, target->span, "invalid assignment target");
    return false;
}

static bool compile_node(Compiler *compiler, const LuneAst *node) {
    if (node == NULL) return false;

    switch (node->kind) {
        case LUNE_AST_PROGRAM:
            return compile_sequence(
                compiler, &node->as.sequence.statements, node->span
            );

        case LUNE_AST_BLOCK: {
            begin_scope(compiler);
            bool ok = compile_sequence(
                compiler, &node->as.sequence.statements, node->span
            );
            end_scope(compiler);
            return ok;
        }

        case LUNE_AST_NULL:
            return emit_op(compiler, LUNE_OP_NULL, node->span);

        case LUNE_AST_BOOL:
            return emit_op(
                compiler,
                node->as.boolean ? LUNE_OP_TRUE : LUNE_OP_FALSE,
                node->span
            );

        case LUNE_AST_NUMBER:
            return compile_number(compiler, node);

        case LUNE_AST_STRING:
            return emit_string_span(compiler, node->span, true);

        case LUNE_AST_NAME:
            return compile_name(compiler, node);

        case LUNE_AST_UNARY:
            if (!compile_node(compiler, node->as.unary.operand)) {
                return false;
            }
            if (node->as.unary.op == LUNE_TOKEN_MINUS) {
                return emit_op(
                    compiler, LUNE_OP_NEGATE, node->span
                );
            }
            if (node->as.unary.op == LUNE_TOKEN_NOT) {
                return emit_op(
                    compiler, LUNE_OP_NOT, node->span
                );
            }
            error_at(
                compiler, node->span, "unsupported unary operator"
            );
            return false;

        case LUNE_AST_BINARY:
            return compile_binary(compiler, node);

        case LUNE_AST_IF:
            return compile_if(compiler, node);

        case LUNE_AST_WHILE:
            return compile_while(compiler, node);

        case LUNE_AST_DECLARE:
            return compile_declare(compiler, node);

        case LUNE_AST_ASSIGN:
            return compile_assign(compiler, node);

        case LUNE_AST_LIST:
            return compile_list(compiler, node);

        case LUNE_AST_MAP:
            return compile_map(compiler, node);

        case LUNE_AST_INDEX:
            return compile_index(compiler, node);

        case LUNE_AST_MEMBER:
            return compile_member(compiler, node);

        case LUNE_AST_FUNCTION:
        case LUNE_AST_CALL:
            error_at(
                compiler,
                node->span,
                "functions are not implemented in the bootstrap VM yet"
            );
            return false;
    }

    return false;
}

bool lune_compile(
    const LuneAst *program,
    const char *source,
    LuneChunk *chunk,
    LuneDiagnosticFn diagnostic,
    void *diagnostic_context
) {
    Compiler compiler = {
        .source = source,
        .chunk = chunk,
        .diagnostic = diagnostic,
        .diagnostic_context = diagnostic_context,
    };

    bool ok = compile_node(&compiler, program);
    if (ok) {
        ok = emit_op(
            &compiler, LUNE_OP_RETURN, program->span
        );
    }

    free(compiler.declared_globals);
    return ok && !compiler.had_error;
}
