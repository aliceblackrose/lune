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

static char *production_stage_path(
    const char *program
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

    const char *suffix =
        "selfhost/stage.lbc";

    size_t directory_length =
        slash == NULL
        ? 0
        : (size_t)(
            slash - canonical + 1
        );

    size_t suffix_length =
        strlen(suffix);

    char *path = malloc(
        directory_length +
        suffix_length +
        1
    );

    if (path == NULL) {
        free(canonical);
        return NULL;
    }

    if (directory_length > 0) {
        memcpy(
            path,
            canonical,
            directory_length
        );
    }

    memcpy(
        path + directory_length,
        suffix,
        suffix_length + 1
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
        print_diagnostic,
        (void *)path
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

static void usage(
    const char *program
) {
    fprintf(
        stderr,
        "usage:\n"
        "  %s FILE [ARGS...]\n"
        "  %s check FILE\n"
        "  %s compile FILE\n"
        "  %s <run|eval|runbc|evalbc> FILE [ARGS...]\n",
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
        strcmp(argv[1], "check") != 0 &&
        strcmp(argv[1], "compile") != 0 &&
        strcmp(argv[1], "run") != 0 &&
        strcmp(argv[1], "eval") != 0 &&
        strcmp(argv[1], "runbc") != 0 &&
        strcmp(argv[1], "evalbc") != 0;

    if (
        !direct_run &&
        (
            strcmp(argv[1], "check") == 0 ||
            strcmp(argv[1], "compile") == 0
        ) &&
        argc != 3
    ) {
        usage(argv[0]);
        return 2;
    }

    if (
        !direct_run &&
        argc < 3
    ) {
        usage(argv[0]);
        return 2;
    }

    char *stage_path =
        production_stage_path(argv[0]);

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

    int result = 2;

    if (direct_run) {
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

    free(stage_path);
    return result;
}
