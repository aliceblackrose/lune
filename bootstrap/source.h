#ifndef LUNE_SOURCE_H
#define LUNE_SOURCE_H

#include <stddef.h>

typedef struct {
    size_t offset;
    size_t length;
    size_t line;
    size_t column;
} LuneSpan;

typedef void (*LuneDiagnosticFn)(
    void *context,
    LuneSpan span,
    const char *message
);

#endif
