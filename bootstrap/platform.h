#ifndef LUNE_PLATFORM_H
#define LUNE_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int status;
    char *stdout_data;
    size_t stdout_length;
    char *stderr_data;
    size_t stderr_length;
} LuneProcessResult;

bool lune_platform_read_file(
    const char *path,
    char **data,
    size_t *length,
    char *error,
    size_t error_capacity
);

bool lune_platform_write_file(
    const char *path,
    const char *data,
    size_t length,
    char *error,
    size_t error_capacity
);

bool lune_platform_canonical_path(
    const char *path,
    char **canonical,
    char *error,
    size_t error_capacity
);

bool lune_platform_temp_file(
    char **path,
    char *error,
    size_t error_capacity
);

bool lune_platform_exec(
    const char *program,
    char *const argv[],
    LuneProcessResult *result,
    char *error,
    size_t error_capacity
);

void lune_platform_process_result_free(
    LuneProcessResult *result
);

#endif
