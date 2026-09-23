#include "bytecode.h"
#include "bytecode_image.h"
#include "platform.h"
#include "value.h"
#include "vm.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *stage_path;
} ProductionCompiler;

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

static bool load_bytecode_file(
    const char *path,
    LuneChunk *chunk,
    char *error,
    size_t error_capacity
) {
    char *data = NULL;
    size_t length = 0;

    if (!lune_platform_read_file(
        path,
        &data,
        &length,
        error,
        error_capacity
    )) {
        return false;
    }

    bool ok = lune_bytecode_load(
        (const uint8_t *)data,
        length,
        chunk,
        error,
        error_capacity
    );

    free(data);
    return ok;
}

static void print_source_excerpt(
    const char *path,
    LuneSpan span
) {
    if (
        path == NULL ||
        path[0] == '<'
    ) {
        return;
    }

    size_t path_length =
        strlen(path);

    if (
        path_length >= 4 &&
        memcmp(
            path + path_length - 4,
            ".lbc",
            4
        ) == 0
    ) {
        return;
    }

    char *source = NULL;
    size_t length = 0;
    char error[256];

    if (!lune_platform_read_file(
        path,
        &source,
        &length,
        error,
        sizeof(error)
    )) {
        return;
    }

    size_t offset =
        span.offset <= length
        ? span.offset
        : length;

    size_t line_start = offset;

    while (
        line_start > 0 &&
        source[line_start - 1] != '\n'
    ) {
        line_start--;
    }

    size_t line_end = offset;

    while (
        line_end < length &&
        source[line_end] != '\n'
    ) {
        line_end++;
    }

    if (
        line_end > line_start &&
        source[line_end - 1] == '\r'
    ) {
        line_end--;
    }

    fputs("  ", stderr);
    fwrite(
        source + line_start,
        1,
        line_end - line_start,
        stderr
    );
    fputc('\n', stderr);

    fputs("  ", stderr);

    for (
        size_t i = line_start;
        i < offset && i < line_end;
        i++
    ) {
        fputc(
            source[i] == '\t'
                ? '\t'
                : ' ',
            stderr
        );
    }

    size_t available =
        line_end > offset
        ? line_end - offset
        : 0;

    size_t marker_length =
        span.length > 0
        ? span.length
        : 1;

    if (
        available > 0 &&
        marker_length > available
    ) {
        marker_length = available;
    }

    if (marker_length == 0) {
        marker_length = 1;
    }

    for (
        size_t i = 0;
        i < marker_length;
        i++
    ) {
        fputc('^', stderr);
    }

    fputc('\n', stderr);
    free(source);
}

static void print_runtime_failure(
    const LuneVM *vm,
    const char *fallback_path
) {
    LuneRuntimeError error;

    if (!lune_vm_last_error(
        vm, &error
    )) {
        return;
    }

    const char *path =
        error.path != NULL
        ? error.path
        : fallback_path;

    fprintf(
        stderr,
        "%s:%zu:%zu: error: %s\n",
        path != NULL
            ? path
            : "<runtime>",
        error.span.line,
        error.span.column,
        error.message
    );

    print_source_excerpt(
        path, error.span
    );

    size_t count =
        lune_vm_trace_count(vm);

    if (count > 1) {
        fputs(
            "stack trace:\n",
            stderr
        );

        for (
            size_t i = 1;
            i < count;
            i++
        ) {
            LuneTraceFrame frame;

            if (!lune_vm_trace_frame(
                vm, i, &frame
            )) {
                continue;
            }

            fprintf(
                stderr,
                "  at %s:%zu:%zu\n",
                frame.path != NULL
                    ? frame.path
                    : "<runtime>",
                frame.span.line,
                frame.span.column
            );
        }
    }
}

static bool run_compiler_stage(
    const ProductionCompiler *compiler,
    const char *source_path,
    const char *output_path,
    bool module,
    char *error,
    size_t error_capacity
) {
    LuneChunk stage;
    lune_chunk_init(&stage);

    if (!load_bytecode_file(
        compiler->stage_path,
        &stage,
        error,
        error_capacity
    )) {
        return false;
    }

    const char *arguments[3] = {
        source_path,
        output_path,
        "module",
    };

    LuneVM *vm = lune_vm_new(
        print_diagnostic,
        (void *)source_path
    );

    if (vm == NULL) {
        lune_chunk_free(&stage);
        (void)snprintf(
            error,
            error_capacity,
            "out of memory"
        );
        return false;
    }

    lune_vm_set_process_args(
        vm,
        module ? 3 : 2,
        arguments
    );

    lune_vm_set_script_path(
        vm,
        compiler->stage_path
    );

    LuneValue result =
        lune_value_null();

    bool ok = lune_vm_run(
        vm,
        &stage,
        &result
    );

    int status = 0;
    bool requested_exit =
        lune_vm_exit_status(
            vm, &status
        );

    lune_vm_free(vm);
    lune_chunk_free(&stage);

    if (!ok) {
        if (
            error_capacity > 0 &&
            error[0] == '\0'
        ) {
            (void)snprintf(
                error,
                error_capacity,
                "self-hosted compiler failed"
            );
        }
        return false;
    }

    if (
        requested_exit &&
        status != 0
    ) {
        (void)snprintf(
            error,
            error_capacity,
            "self-hosted compiler exited with status %d",
            status
        );
        return false;
    }

    return true;
}

static bool compile_source(
    void *context,
    const char *path,
    bool module,
    LuneChunk *chunk,
    char *error,
    size_t error_capacity
) {
    ProductionCompiler *compiler =
        context;

    if (
        error != NULL &&
        error_capacity > 0
    ) {
        error[0] = '\0';
    }

    char *temporary = NULL;

    if (!lune_platform_temp_file(
        &temporary,
        error,
        error_capacity
    )) {
        return false;
    }

    bool ok = run_compiler_stage(
        compiler,
        path,
        temporary,
        module,
        error,
        error_capacity
    );

    if (ok) {
        ok = load_bytecode_file(
            temporary,
            chunk,
            error,
            error_capacity
        );
    }

    (void)remove(temporary);
    free(temporary);
    return ok;
}

static char *production_tool_path(
    const char *program,
    const char *name
) {
    char error[256];
    char *canonical = NULL;

    if (!lune_platform_canonical_path(
        program,
        &canonical,
        error,
        sizeof(error)
    )) {
        size_t length = strlen(program);
        canonical = malloc(length + 1);

        if (canonical == NULL) {
            return NULL;
        }

        memcpy(
            canonical,
            program,
            length + 1
        );
    }

    char *slash =
        strrchr(canonical, '/');

#if defined(_WIN32)
    char *backslash =
        strrchr(canonical, '\\');

    if (
        backslash != NULL &&
        (
            slash == NULL ||
            backslash > slash
        )
    ) {
        slash = backslash;
    }
#endif

    static const char prefix[] =
        "selfhost/";

    size_t directory_length =
        slash == NULL
        ? 0
        : (size_t)(
            slash - canonical + 1
        );

    size_t prefix_length =
        sizeof(prefix) - 1;

    size_t name_length =
        strlen(name);

    if (
        directory_length >
            SIZE_MAX -
                prefix_length -
                name_length -
                1
    ) {
        free(canonical);
        return NULL;
    }

    char *path = malloc(
        directory_length +
        prefix_length +
        name_length +
        1
    );

    if (path == NULL) {
        free(canonical);
        return NULL;
    }

    size_t offset = 0;

    if (directory_length > 0) {
        memcpy(
            path,
            canonical,
            directory_length
        );
        offset = directory_length;
    }

    memcpy(
        path + offset,
        prefix,
        prefix_length
    );
    offset += prefix_length;

    memcpy(
        path + offset,
        name,
        name_length + 1
    );

    free(canonical);
    return path;
}
static int execute_chunk(
    const char *path,
    LuneChunk *chunk,
    ProductionCompiler *compiler,
    bool print_result,
    int process_argc,
    const char *const *process_argv
) {
    LuneVM *vm = lune_vm_new(
        NULL,
        NULL
    );

    if (vm == NULL) {
        fprintf(
            stderr,
            "%s: error: out of memory\n",
            path
        );
        return 1;
    }

    lune_vm_set_process_args(
        vm,
        process_argc,
        process_argv
    );

    lune_vm_set_script_path(
        vm, path
    );

    lune_vm_set_source_compiler(
        vm,
        compile_source,
        compiler
    );

    LuneValue result =
        lune_value_null();

    bool ok = lune_vm_run(
        vm,
        chunk,
        &result
    );

    if (!ok) {
        print_runtime_failure(
            vm, path
        );
    }

    int exit_status = 0;
    bool requested_exit =
        lune_vm_exit_status(
            vm, &exit_status
        );

    if (
        ok &&
        print_result &&
        !requested_exit
    ) {
        lune_value_print(
            stdout, result
        );
        putchar('\n');
    }

    lune_vm_free(vm);

    if (!ok) return 1;

    return requested_exit
        ? exit_status
        : 0;
}

static int compile_and_maybe_run(
    const char *path,
    ProductionCompiler *compiler,
    bool execute,
    bool print_result,
    int process_argc,
    const char *const *process_argv
) {
    LuneChunk chunk;
    lune_chunk_init(&chunk);

    char error[256] = {0};

    if (!compile_source(
        compiler,
        path,
        false,
        &chunk,
        error,
        sizeof(error)
    )) {
        if (error[0] != '\0') {
            fprintf(
                stderr,
                "%s: error: %s\n",
                path,
                error
            );
        }

        lune_chunk_free(&chunk);
        return 1;
    }

    int status = 0;

    if (execute) {
        status = execute_chunk(
            path,
            &chunk,
            compiler,
            print_result,
            process_argc,
            process_argv
        );
    }

    lune_chunk_free(&chunk);
    return status;
}

static int execute_bytecode(
    const char *path,
    ProductionCompiler *compiler,
    bool print_result,
    int process_argc,
    const char *const *process_argv
) {
    LuneChunk chunk;
    lune_chunk_init(&chunk);

    char error[256] = {0};

    if (!load_bytecode_file(
        path,
        &chunk,
        error,
        sizeof(error)
    )) {
        fprintf(
            stderr,
            "%s: error: %s\n",
            path,
            error
        );
        return 1;
    }

    int status = execute_chunk(
        path,
        &chunk,
        compiler,
        print_result,
        process_argc,
        process_argv
    );

    lune_chunk_free(&chunk);
    return status;
}

static bool ends_with(
    const char *text,
    const char *suffix
) {
    size_t text_length =
        strlen(text);
    size_t suffix_length =
        strlen(suffix);

    return
        text_length >= suffix_length &&
        memcmp(
            text +
                text_length -
                suffix_length,
            suffix,
            suffix_length
        ) == 0;
}

static char *compile_output_path(
    const char *source
) {
    size_t length = strlen(source);
    size_t stem = length;

    if (
        length >= 5 &&
        memcmp(
            source + length - 5,
            ".lune",
            5
        ) == 0
    ) {
        stem = length - 5;
    }

    static const char extension[] =
        ".lbc";

    char *output = malloc(
        stem +
        sizeof(extension)
    );

    if (output == NULL) {
        return NULL;
    }

    memcpy(
        output,
        source,
        stem
    );

    memcpy(
        output + stem,
        extension,
        sizeof(extension)
    );

    return output;
}

static int compile_file(
    const char *path,
    ProductionCompiler *compiler
) {
    char *output =
        compile_output_path(path);

    if (output == NULL) {
        fprintf(
            stderr,
            "%s: error: out of memory\n",
            path
        );
        return 1;
    }

    char error[256] = {0};

    bool ok = run_compiler_stage(
        compiler,
        path,
        output,
        false,
        error,
        sizeof(error)
    );

    if (!ok && error[0] != '\0') {
        fprintf(
            stderr,
            "%s: error: %s\n",
            path,
            error
        );
    }

    free(output);
    return ok ? 0 : 1;
}

typedef struct {
    char *data;
    size_t count;
    size_t capacity;
} ReplBuffer;

typedef struct {
    LuneChunk **items;
    size_t count;
    size_t capacity;
} ReplChunks;

static void repl_buffer_clear(
    ReplBuffer *buffer
) {
    buffer->count = 0;

    if (buffer->data != NULL) {
        buffer->data[0] = '\0';
    }
}

static bool repl_buffer_append(
    ReplBuffer *buffer,
    const char *text,
    size_t length
) {
    if (
        length >
        SIZE_MAX -
            buffer->count -
            1
    ) {
        return false;
    }

    size_t needed =
        buffer->count +
        length +
        1;

    if (needed > buffer->capacity) {
        size_t next =
            buffer->capacity == 0
            ? 256
            : buffer->capacity;

        while (next < needed) {
            if (next > SIZE_MAX / 2) {
                next = needed;
                break;
            }

            next *= 2;
        }

        char *grown = realloc(
            buffer->data,
            next
        );

        if (grown == NULL) {
            return false;
        }

        buffer->data = grown;
        buffer->capacity = next;
    }

    memcpy(
        buffer->data + buffer->count,
        text,
        length
    );

    buffer->count += length;
    buffer->data[buffer->count] = '\0';
    return true;
}

static char *repl_read_line(
    const char *prompt
) {
    fputs(prompt, stderr);
    fflush(stderr);

    size_t capacity = 128;
    size_t count = 0;
    char *line = malloc(capacity);

    if (line == NULL) {
        return NULL;
    }

    int c;

    while (
        (c = fgetc(stdin)) != EOF
    ) {
        if (count + 2 > capacity) {
            if (
                capacity >
                SIZE_MAX / 2
            ) {
                free(line);
                return NULL;
            }

            capacity *= 2;
            char *grown = realloc(
                line, capacity
            );

            if (grown == NULL) {
                free(line);
                return NULL;
            }

            line = grown;
        }

        line[count++] = (char)c;

        if (c == '\n') {
            break;
        }
    }

    if (
        c == EOF &&
        count == 0
    ) {
        free(line);
        return NULL;
    }

    if (
        count == 0 ||
        line[count - 1] != '\n'
    ) {
        line[count++] = '\n';
    }

    line[count] = '\0';
    return line;
}

static int repl_delimiter_depth(
    const char *text,
    size_t length
) {
    int depth = 0;
    bool string = false;
    bool escape = false;
    bool comment = false;

    for (
        size_t i = 0;
        i < length;
        i++
    ) {
        unsigned char c =
            (unsigned char)text[i];

        if (comment) {
            if (c == '\n') {
                comment = false;
            }
            continue;
        }

        if (string) {
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                string = false;
            }
            continue;
        }

        if (
            c == '/' &&
            i + 1 < length &&
            text[i + 1] == '/'
        ) {
            comment = true;
            i++;
            continue;
        }

        if (c == '"') {
            string = true;
        } else if (
            c == '(' ||
            c == '[' ||
            c == '{'
        ) {
            depth++;
        } else if (
            c == ')' ||
            c == ']' ||
            c == '}'
        ) {
            depth--;
        }
    }

    return depth;
}

static bool repl_chunks_push(
    ReplChunks *chunks,
    LuneChunk *chunk
) {
    if (
        chunks->count ==
        chunks->capacity
    ) {
        size_t next =
            chunks->capacity == 0
            ? 16
            : chunks->capacity * 2;

        if (
            next <
                chunks->capacity ||
            next >
                SIZE_MAX /
                sizeof(*chunks->items)
        ) {
            return false;
        }

        LuneChunk **grown = realloc(
            chunks->items,
            next * sizeof(*grown)
        );

        if (grown == NULL) {
            return false;
        }

        chunks->items = grown;
        chunks->capacity = next;
    }

    chunks->items[
        chunks->count++
    ] = chunk;

    return true;
}

static void repl_chunks_free(
    ReplChunks *chunks
) {
    for (
        size_t i = 0;
        i < chunks->count;
        i++
    ) {
        lune_chunk_free(
            chunks->items[i]
        );
        free(chunks->items[i]);
    }

    free(chunks->items);
    *chunks = (ReplChunks){0};
}

static int run_repl(
    ProductionCompiler *compiler
) {
    char error[256] = {0};
    char *source_path = NULL;

    if (!lune_platform_temp_file(
        &source_path,
        error,
        sizeof(error)
    )) {
        fprintf(
            stderr,
            "lune: error: %s\n",
            error
        );
        return 1;
    }

    LuneVM *vm = lune_vm_new(
        print_diagnostic,
        (void *)"<repl>"
    );

    if (vm == NULL) {
        (void)remove(source_path);
        free(source_path);
        fputs(
            "lune: error: out of memory\n",
            stderr
        );
        return 1;
    }

    lune_vm_set_script_path(
        vm, "repl.lune"
    );

    lune_vm_set_source_compiler(
        vm,
        compile_source,
        compiler
    );

    ReplBuffer source = {0};
    ReplChunks chunks = {0};
    int status = 0;

    for (;;) {
        const char *prompt =
            source.count == 0
            ? "> "
            : "... ";

        char *line =
            repl_read_line(prompt);

        if (line == NULL) {
            break;
        }

        if (
            source.count == 0 &&
            (
                strcmp(line, ":quit\n") == 0 ||
                strcmp(line, ":exit\n") == 0
            )
        ) {
            free(line);
            break;
        }

        size_t line_length =
            strlen(line);

        if (
            source.count == 0 &&
            (
                line_length == 1 &&
                line[0] == '\n'
            )
        ) {
            free(line);
            continue;
        }

        if (!repl_buffer_append(
            &source,
            line,
            line_length
        )) {
            free(line);
            status = 1;
            fputs(
                "lune: error: out of memory\n",
                stderr
            );
            break;
        }

        free(line);

        if (
            repl_delimiter_depth(
                source.data,
                source.count
            ) > 0
        ) {
            continue;
        }

        if (!lune_platform_write_file(
            source_path,
            source.data,
            source.count,
            error,
            sizeof(error)
        )) {
            fprintf(
                stderr,
                "lune: error: %s\n",
                error
            );
            status = 1;
            repl_buffer_clear(&source);
            continue;
        }

        LuneChunk *chunk =
            malloc(sizeof(*chunk));

        if (chunk == NULL) {
            status = 1;
            fputs(
                "lune: error: out of memory\n",
                stderr
            );
            break;
        }

        lune_chunk_init(chunk);
        error[0] = '\0';

        if (!compile_source(
            compiler,
            source_path,
            false,
            chunk,
            error,
            sizeof(error)
        )) {
            if (error[0] != '\0') {
                fprintf(
                    stderr,
                    "<repl>: error: %s\n",
                    error
                );
            }

            lune_chunk_free(chunk);
            free(chunk);
            repl_buffer_clear(&source);
            continue;
        }

        if (!repl_chunks_push(
            &chunks, chunk
        )) {
            lune_chunk_free(chunk);
            free(chunk);
            status = 1;
            fputs(
                "lune: error: out of memory\n",
                stderr
            );
            break;
        }

        LuneValue result =
            lune_value_null();

        bool ok =
            lune_vm_run_incremental(
                vm,
                chunk,
                &result
            );

        int exit_status = 0;
        bool requested_exit =
            lune_vm_exit_status(
                vm, &exit_status
            );

        if (ok && !requested_exit) {
            lune_value_print(
                stdout, result
            );
            putchar('\n');
            fflush(stdout);
        }

        repl_buffer_clear(&source);

        if (requested_exit) {
            status = exit_status;
            break;
        }
    }

    lune_vm_free(vm);
    repl_chunks_free(&chunks);
    free(source.data);
    (void)remove(source_path);
    free(source_path);
    return status;
}

static void usage(
    const char *program
) {
    fprintf(
        stderr,
        "usage:\n"
        "  %s FILE [ARGS...]\n"
        "  %s repl\n"
        "  %s check FILE\n"
        "  %s fmt FILE\n"
        "  %s compile FILE\n"
        "  %s <run|eval|runbc|evalbc> FILE [ARGS...]\n",
        program,
        program,
        program,
        program,
        program,
        program
    );
}

int main(
    int argc,
    char **argv
) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    bool direct_run =
        strcmp(argv[1], "repl") != 0 &&
        strcmp(argv[1], "check") != 0 &&
        strcmp(argv[1], "fmt") != 0 &&
        strcmp(argv[1], "compile") != 0 &&
        strcmp(argv[1], "run") != 0 &&
        strcmp(argv[1], "eval") != 0 &&
        strcmp(argv[1], "runbc") != 0 &&
        strcmp(argv[1], "evalbc") != 0;

    bool repl_command =
        strcmp(argv[1], "repl") == 0;

    if (
        repl_command &&
        argc != 2
    ) {
        usage(argv[0]);
        return 2;
    }

    if (
        !direct_run &&
        !repl_command &&
        (
            strcmp(argv[1], "check") == 0 ||
            strcmp(argv[1], "fmt") == 0 ||
            strcmp(argv[1], "compile") == 0
        ) &&
        argc != 3
    ) {
        usage(argv[0]);
        return 2;
    }

    if (
        !direct_run &&
        !repl_command &&
        argc < 3
    ) {
        usage(argv[0]);
        return 2;
    }

    char *stage_path =
        production_tool_path(
            argv[0], "stage.lbc"
        );

    if (stage_path == NULL) {
        fprintf(
            stderr,
            "lune: error: unable to locate compiler bytecode\n"
        );
        return 1;
    }

    ProductionCompiler compiler = {
        .stage_path = stage_path,
    };

    char *fmt_path =
        production_tool_path(
            argv[0], "fmt.lbc"
        );

    if (fmt_path == NULL) {
        free(stage_path);
        fprintf(
            stderr,
            "lune: error: unable to locate formatter bytecode\n"
        );
        return 1;
    }

    int result = 2;

    if (repl_command) {
        result = run_repl(
            &compiler
        );
    } else if (direct_run) {
        if (ends_with(
            argv[1], ".lbc"
        )) {
            result = execute_bytecode(
                argv[1],
                &compiler,
                false,
                argc - 2,
                (const char *const *)
                    (argv + 2)
            );
        } else {
            result = compile_and_maybe_run(
                argv[1],
                &compiler,
                true,
                false,
                argc - 2,
                (const char *const *)
                    (argv + 2)
            );
        }
    } else if (
        strcmp(argv[1], "check") == 0
    ) {
        result = compile_and_maybe_run(
            argv[2],
            &compiler,
            false,
            false,
            0,
            NULL
        );
    } else if (
        strcmp(argv[1], "fmt") == 0
    ) {
        result = compile_and_maybe_run(
            argv[2],
            &compiler,
            false,
            false,
            0,
            NULL
        );

        if (result == 0) {
            const char *fmt_args[1] = {
                argv[2],
            };

            result = execute_bytecode(
                fmt_path,
                &compiler,
                false,
                1,
                fmt_args
            );
        }
    } else if (
        strcmp(argv[1], "compile") == 0
    ) {
        result = compile_file(
            argv[2],
            &compiler
        );
    } else if (
        strcmp(argv[1], "run") == 0
    ) {
        result = compile_and_maybe_run(
            argv[2],
            &compiler,
            true,
            false,
            argc - 3,
            (const char *const *)
                (argv + 3)
        );
    } else if (
        strcmp(argv[1], "eval") == 0
    ) {
        result = compile_and_maybe_run(
            argv[2],
            &compiler,
            true,
            true,
            argc - 3,
            (const char *const *)
                (argv + 3)
        );
    } else if (
        strcmp(argv[1], "runbc") == 0
    ) {
        result = execute_bytecode(
            argv[2],
            &compiler,
            false,
            argc - 3,
            (const char *const *)
                (argv + 3)
        );
    } else if (
        strcmp(argv[1], "evalbc") == 0
    ) {
        result = execute_bytecode(
            argv[2],
            &compiler,
            true,
            argc - 3,
            (const char *const *)
                (argv + 3)
        );
    } else {
        usage(argv[0]);
    }

    free(fmt_path);
    free(stage_path);
    return result;
}
