#define _POSIX_C_SOURCE 200809L

#include "platform.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static void set_error(
    char *error,
    size_t capacity,
    const char *format,
    ...
) {
    if (
        error == NULL ||
        capacity == 0
    ) {
        return;
    }

    va_list args;
    va_start(args, format);
    (void)vsnprintf(
        error,
        capacity,
        format,
        args
    );
    va_end(args);
}

bool lune_platform_read_file(
    const char *path,
    char **data,
    size_t *length,
    char *error,
    size_t error_capacity
) {
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        set_error(
            error,
            error_capacity,
            "unable to open %s: %s",
            path,
            strerror(errno)
        );
        return false;
    }

    if (
        fseek(file, 0, SEEK_END) != 0
    ) {
        set_error(
            error,
            error_capacity,
            "unable to seek %s: %s",
            path,
            strerror(errno)
        );
        fclose(file);
        return false;
    }

    long size = ftell(file);

    if (
        size < 0 ||
        fseek(file, 0, SEEK_SET) != 0
    ) {
        set_error(
            error,
            error_capacity,
            "unable to measure %s: %s",
            path,
            strerror(errno)
        );
        fclose(file);
        return false;
    }

    size_t bytes = (size_t)size;

    if (
        (long)bytes != size ||
        bytes == SIZE_MAX
    ) {
        set_error(
            error,
            error_capacity,
            "file is too large: %s",
            path
        );
        fclose(file);
        return false;
    }

    char *buffer =
        malloc(bytes + 1);

    if (buffer == NULL) {
        set_error(
            error,
            error_capacity,
            "out of memory reading %s",
            path
        );
        fclose(file);
        return false;
    }

    size_t read =
        fread(buffer, 1, bytes, file);

    if (
        read != bytes ||
        ferror(file)
    ) {
        set_error(
            error,
            error_capacity,
            "unable to read %s: %s",
            path,
            ferror(file)
                ? strerror(errno)
                : "short read"
        );
        free(buffer);
        fclose(file);
        return false;
    }

    buffer[bytes] = '\0';

    if (fclose(file) != 0) {
        set_error(
            error,
            error_capacity,
            "unable to close %s: %s",
            path,
            strerror(errno)
        );
        free(buffer);
        return false;
    }

    *data = buffer;
    *length = bytes;
    return true;
}

bool lune_platform_write_file(
    const char *path,
    const char *data,
    size_t length,
    char *error,
    size_t error_capacity
) {
    FILE *file = fopen(path, "wb");

    if (file == NULL) {
        set_error(
            error,
            error_capacity,
            "unable to open %s: %s",
            path,
            strerror(errno)
        );
        return false;
    }

    size_t written =
        fwrite(data, 1, length, file);

    if (
        written != length ||
        ferror(file)
    ) {
        set_error(
            error,
            error_capacity,
            "unable to write %s: %s",
            path,
            strerror(errno)
        );
        fclose(file);
        return false;
    }

    if (fclose(file) != 0) {
        set_error(
            error,
            error_capacity,
            "unable to close %s: %s",
            path,
            strerror(errno)
        );
        return false;
    }

    return true;
}

#if !defined(_WIN32)

static bool read_capture(
    FILE *file,
    char **data,
    size_t *length,
    char *error,
    size_t error_capacity
) {
    if (
        fseek(file, 0, SEEK_END) != 0
    ) {
        set_error(
            error,
            error_capacity,
            "unable to seek process output: %s",
            strerror(errno)
        );
        return false;
    }

    long size = ftell(file);

    if (
        size < 0 ||
        fseek(file, 0, SEEK_SET) != 0
    ) {
        set_error(
            error,
            error_capacity,
            "unable to measure process output: %s",
            strerror(errno)
        );
        return false;
    }

    size_t bytes = (size_t)size;

    if (
        (long)bytes != size ||
        bytes == SIZE_MAX
    ) {
        set_error(
            error,
            error_capacity,
            "process output is too large"
        );
        return false;
    }

    char *buffer =
        malloc(bytes + 1);

    if (buffer == NULL) {
        set_error(
            error,
            error_capacity,
            "out of memory reading process output"
        );
        return false;
    }

    size_t read =
        fread(buffer, 1, bytes, file);

    if (
        read != bytes ||
        ferror(file)
    ) {
        set_error(
            error,
            error_capacity,
            "unable to read process output"
        );
        free(buffer);
        return false;
    }

    buffer[bytes] = '\0';
    *data = buffer;
    *length = bytes;
    return true;
}

bool lune_platform_exec(
    const char *program,
    char *const argv[],
    LuneProcessResult *result,
    char *error,
    size_t error_capacity
) {
    *result = (LuneProcessResult){0};

    FILE *stdout_file = tmpfile();

    if (stdout_file == NULL) {
        set_error(
            error,
            error_capacity,
            "unable to create stdout capture: %s",
            strerror(errno)
        );
        return false;
    }

    FILE *stderr_file = tmpfile();

    if (stderr_file == NULL) {
        set_error(
            error,
            error_capacity,
            "unable to create stderr capture: %s",
            strerror(errno)
        );
        fclose(stdout_file);
        return false;
    }

    pid_t pid = fork();

    if (pid < 0) {
        set_error(
            error,
            error_capacity,
            "unable to start process: %s",
            strerror(errno)
        );
        fclose(stderr_file);
        fclose(stdout_file);
        return false;
    }

    if (pid == 0) {
        int out_fd = fileno(stdout_file);
        int err_fd = fileno(stderr_file);

        if (
            out_fd < 0 ||
            err_fd < 0 ||
            dup2(
                out_fd,
                STDOUT_FILENO
            ) < 0 ||
            dup2(
                err_fd,
                STDERR_FILENO
            ) < 0
        ) {
            _exit(126);
        }

        execvp(program, argv);

        char message[256];
        int written = snprintf(
            message,
            sizeof(message),
            "exec %s failed: %s\n",
            program,
            strerror(errno)
        );

        if (written > 0) {
            size_t count =
                (size_t)written;

            if (
                count >=
                sizeof(message)
            ) {
                count =
                    sizeof(message) - 1;
            }

            ssize_t write_result =
                write(
                    STDERR_FILENO,
                    message,
                    count
                );

            (void)write_result;
        }

        _exit(127);
    }

    int wait_status = 0;

    while (
        waitpid(
            pid,
            &wait_status,
            0
        ) < 0
    ) {
        if (errno == EINTR) {
            continue;
        }

        set_error(
            error,
            error_capacity,
            "unable to wait for process: %s",
            strerror(errno)
        );

        fclose(stderr_file);
        fclose(stdout_file);
        return false;
    }

    if (WIFEXITED(wait_status)) {
        result->status =
            WEXITSTATUS(wait_status);
    } else if (
        WIFSIGNALED(wait_status)
    ) {
        result->status =
            128 +
            WTERMSIG(wait_status);
    } else {
        result->status = 1;
    }

    bool ok =
        read_capture(
            stdout_file,
            &result->stdout_data,
            &result->stdout_length,
            error,
            error_capacity
        ) &&
        read_capture(
            stderr_file,
            &result->stderr_data,
            &result->stderr_length,
            error,
            error_capacity
        );

    fclose(stderr_file);
    fclose(stdout_file);

    if (!ok) {
        lune_platform_process_result_free(
            result
        );
        return false;
    }

    return true;
}

#else

bool lune_platform_exec(
    const char *program,
    char *const argv[],
    LuneProcessResult *result,
    char *error,
    size_t error_capacity
) {
    (void)program;
    (void)argv;
    *result = (LuneProcessResult){0};

    set_error(
        error,
        error_capacity,
        "child processes are not implemented on this platform"
    );
    return false;
}

#endif

void lune_platform_process_result_free(
    LuneProcessResult *result
) {
    free(result->stdout_data);
    free(result->stderr_data);
    *result = (LuneProcessResult){0};
}
