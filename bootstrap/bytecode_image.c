#include "bytecode_image.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LUNE_BC_MAGIC "LUNEBC01"
#define LUNE_BC_MAGIC_LENGTH 8

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t offset;
    char *error;
    size_t error_capacity;
} Reader;

static bool fail(
    Reader *reader,
    const char *message
) {
    if (
        reader->error != NULL &&
        reader->error_capacity > 0
    ) {
        (void)snprintf(
            reader->error,
            reader->error_capacity,
            "%s",
            message
        );
    }
    return false;
}

static bool read_u8(
    Reader *reader,
    uint8_t *value
) {
    if (
        reader->offset >=
        reader->length
    ) {
        return fail(
            reader,
            "truncated bytecode image"
        );
    }

    *value =
        reader->data[
            reader->offset++
        ];
    return true;
}

static bool read_u16(
    Reader *reader,
    uint16_t *value
) {
    if (
        reader->length -
            reader->offset < 2
    ) {
        return fail(
            reader,
            "truncated bytecode image"
        );
    }

    const uint8_t *p =
        reader->data +
        reader->offset;

    *value = (uint16_t)(
        (uint16_t)p[0] |
        ((uint16_t)p[1] << 8)
    );

    reader->offset += 2;
    return true;
}

static bool read_u32(
    Reader *reader,
    uint32_t *value
) {
    if (
        reader->length -
            reader->offset < 4
    ) {
        return fail(
            reader,
            "truncated bytecode image"
        );
    }

    const uint8_t *p =
        reader->data +
        reader->offset;

    *value =
        (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);

    reader->offset += 4;
    return true;
}

static bool read_blob(
    Reader *reader,
    size_t length,
    const uint8_t **data
) {
    if (
        length >
        reader->length -
            reader->offset
    ) {
        return fail(
            reader,
            "truncated bytecode image"
        );
    }

    *data =
        reader->data +
        reader->offset;

    reader->offset += length;
    return true;
}

static bool read_text(
    Reader *reader,
    char **text,
    size_t *length
) {
    uint32_t encoded_length;

    if (!read_u32(
        reader,
        &encoded_length
    )) {
        return false;
    }

    const uint8_t *data;

    if (!read_blob(
        reader,
        encoded_length,
        &data
    )) {
        return false;
    }

    size_t size =
        (size_t)encoded_length;

    if (size == SIZE_MAX) {
        return fail(
            reader,
            "bytecode string is too large"
        );
    }

    char *copy =
        malloc(size + 1);

    if (copy == NULL) {
        return fail(
            reader,
            "out of memory"
        );
    }

    memcpy(copy, data, size);
    copy[size] = '\0';

    *text = copy;
    *length = size;
    return true;
}

static bool read_span(
    Reader *reader,
    LuneSpan *span
) {
    uint32_t offset;
    uint32_t length;
    uint32_t line;
    uint32_t column;

    if (
        !read_u32(reader, &offset) ||
        !read_u32(reader, &length) ||
        !read_u32(reader, &line) ||
        !read_u32(reader, &column)
    ) {
        return false;
    }

    *span = (LuneSpan){
        .offset = (size_t)offset,
        .length = (size_t)length,
        .line = (size_t)line,
        .column = (size_t)column,
    };

    return true;
}

static bool read_constant(
    Reader *reader,
    LuneValue *value
) {
    uint8_t tag;
    char *text = NULL;
    size_t length = 0;

    if (
        !read_u8(reader, &tag) ||
        !read_text(
            reader,
            &text,
            &length
        )
    ) {
        free(text);
        return false;
    }

    if (memchr(text, '\0', length) != NULL) {
        free(text);
        return fail(
            reader,
            "invalid numeric constant"
        );
    }

    errno = 0;
    char *end = NULL;

    if (tag == 0) {
        long long number =
            strtoll(text, &end, 10);

        if (
            errno == ERANGE ||
            end == text ||
            (size_t)(end - text) != length
        ) {
            free(text);
            return fail(
                reader,
                "invalid integer constant"
            );
        }

        *value = lune_value_int(
            (int64_t)number
        );
    } else if (tag == 1) {
        double number =
            strtod(text, &end);

        if (
            errno == ERANGE ||
            end == text ||
            (size_t)(end - text) != length
        ) {
            free(text);
            return fail(
                reader,
                "invalid float constant"
            );
        }

        *value =
            lune_value_float(number);
    } else {
        free(text);
        return fail(
            reader,
            "unknown constant tag"
        );
    }

    free(text);
    return true;
}

static bool load_chunk(
    Reader *reader,
    LuneChunk *chunk
);

static bool read_function(
    Reader *reader,
    LuneFunction **function_out
) {
    uint16_t arity;
    uint16_t upvalue_count;

    if (
        !read_u16(
            reader, &arity
        ) ||
        !read_u16(
            reader,
            &upvalue_count
        )
    ) {
        return false;
    }

    LuneFunction *function =
        lune_function_new(arity);

    if (function == NULL) {
        return fail(
            reader,
            "out of memory"
        );
    }

    LuneUpvalueDesc *upvalues =
        NULL;

    if (upvalue_count > 0) {
        upvalues = malloc(
            (size_t)upvalue_count *
            sizeof(*upvalues)
        );

        if (upvalues == NULL) {
            lune_function_free(
                function
            );
            return fail(
                reader,
                "out of memory"
            );
        }
    }

    for (
        uint16_t i = 0;
        i < upvalue_count;
        i++
    ) {
        uint8_t local;
        uint16_t index;

        if (
            !read_u8(
                reader, &local
            ) ||
            !read_u16(
                reader, &index
            )
        ) {
            free(upvalues);
            lune_function_free(
                function
            );
            return false;
        }

        if (local > 1) {
            free(upvalues);
            lune_function_free(
                function
            );
            return fail(
                reader,
                "invalid upvalue descriptor"
            );
        }

        upvalues[i] =
            (LuneUpvalueDesc){
                .is_local =
                    local != 0,
                .index = index,
            };
    }

    if (!lune_function_set_upvalues(
        function,
        upvalues,
        upvalue_count
    )) {
        free(upvalues);
        lune_function_free(
            function
        );
        return fail(
            reader,
            "out of memory"
        );
    }

    free(upvalues);

    if (!load_chunk(
        reader,
        &function->chunk
    )) {
        lune_function_free(function);
        return false;
    }

    *function_out = function;
    return true;
}

static bool load_chunk(
    Reader *reader,
    LuneChunk *chunk
) {
    lune_chunk_init(chunk);

    uint32_t code_count;

    if (!read_u32(
        reader, &code_count
    )) {
        return false;
    }

    const uint8_t *code;

    if (!read_blob(
        reader,
        (size_t)code_count,
        &code
    )) {
        return false;
    }

    if (code_count > 0) {
        chunk->code = malloc(
            (size_t)code_count
        );

        chunk->spans = malloc(
            (size_t)code_count *
            sizeof(*chunk->spans)
        );

        if (
            chunk->code == NULL ||
            chunk->spans == NULL
        ) {
            lune_chunk_free(chunk);
            return fail(
                reader,
                "out of memory"
            );
        }

        memcpy(
            chunk->code,
            code,
            (size_t)code_count
        );
    }

    chunk->count =
        (size_t)code_count;
    chunk->capacity =
        (size_t)code_count;

    for (
        uint32_t i = 0;
        i < code_count;
        i++
    ) {
        if (!read_span(
            reader,
            &chunk->spans[i]
        )) {
            lune_chunk_free(chunk);
            return false;
        }
    }

    uint32_t constant_count;

    if (
        !read_u32(
            reader,
            &constant_count
        ) ||
        constant_count >
            UINT16_MAX
    ) {
        lune_chunk_free(chunk);

        if (
            constant_count >
            UINT16_MAX
        ) {
            return fail(
                reader,
                "too many constants in bytecode image"
            );
        }

        return false;
    }

    for (
        uint32_t i = 0;
        i < constant_count;
        i++
    ) {
        LuneValue value;
        uint16_t index;

        if (
            !read_constant(
                reader, &value
            ) ||
            !lune_chunk_add_constant(
                chunk,
                value,
                &index
            ) ||
            index != i
        ) {
            lune_chunk_free(chunk);

            if (
                reader->error != NULL &&
                reader->error[0] == '\0'
            ) {
                (void)fail(
                    reader,
                    "invalid constant table"
                );
            }

            return false;
        }
    }

    uint32_t name_count;

    if (
        !read_u32(
            reader,
            &name_count
        ) ||
        name_count >
            UINT16_MAX
    ) {
        lune_chunk_free(chunk);

        if (
            name_count >
            UINT16_MAX
        ) {
            return fail(
                reader,
                "too many names in bytecode image"
            );
        }

        return false;
    }

    for (
        uint32_t i = 0;
        i < name_count;
        i++
    ) {
        char *name = NULL;
        size_t length = 0;
        uint16_t index;

        if (
            !read_text(
                reader,
                &name,
                &length
            ) ||
            !lune_chunk_intern_name(
                chunk,
                name,
                length,
                &index
            ) ||
            index != i
        ) {
            free(name);
            lune_chunk_free(chunk);

            if (
                reader->error != NULL &&
                reader->error[0] == '\0'
            ) {
                (void)fail(
                    reader,
                    "invalid name table"
                );
            }

            return false;
        }

        free(name);
    }

    uint32_t function_count;

    if (
        !read_u32(
            reader,
            &function_count
        ) ||
        function_count >
            UINT16_MAX
    ) {
        lune_chunk_free(chunk);

        if (
            function_count >
            UINT16_MAX
        ) {
            return fail(
                reader,
                "too many functions in bytecode image"
            );
        }

        return false;
    }

    for (
        uint32_t i = 0;
        i < function_count;
        i++
    ) {
        LuneFunction *function =
            NULL;
        uint16_t index;

        if (
            !read_function(
                reader,
                &function
            ) ||
            !lune_chunk_add_function(
                chunk,
                function,
                &index
            ) ||
            index != i
        ) {
            lune_function_free(
                function
            );
            lune_chunk_free(chunk);

            if (
                reader->error != NULL &&
                reader->error[0] == '\0'
            ) {
                (void)fail(
                    reader,
                    "invalid function table"
                );
            }

            return false;
        }
    }

    return true;
}

bool lune_bytecode_load(
    const uint8_t *data,
    size_t length,
    LuneChunk *chunk,
    char *error,
    size_t error_capacity
) {
    if (
        error != NULL &&
        error_capacity > 0
    ) {
        error[0] = '\0';
    }

    Reader reader = {
        .data = data,
        .length = length,
        .error = error,
        .error_capacity =
            error_capacity,
    };

    if (
        length <
            LUNE_BC_MAGIC_LENGTH ||
        memcmp(
            data,
            LUNE_BC_MAGIC,
            LUNE_BC_MAGIC_LENGTH
        ) != 0
    ) {
        return fail(
            &reader,
            "invalid bytecode image header"
        );
    }

    reader.offset =
        LUNE_BC_MAGIC_LENGTH;

    if (!load_chunk(
        &reader, chunk
    )) {
        return false;
    }

    if (
        reader.offset !=
        reader.length
    ) {
        lune_chunk_free(chunk);
        return fail(
            &reader,
            "trailing data in bytecode image"
        );
    }

    return true;
}
