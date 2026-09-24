#ifndef LUNE_VALUE_H
#define LUNE_VALUE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct LuneObj LuneObj;

typedef enum {
    LUNE_VALUE_NULL,
    LUNE_VALUE_BOOL,
    LUNE_VALUE_INT,
    LUNE_VALUE_FLOAT,
    LUNE_VALUE_OBJ
} LuneValueKind;

typedef struct {
    LuneValueKind kind;
    union {
        bool boolean;
        int64_t integer;
        double floating;
        LuneObj *object;
    } as;
} LuneValue;

static inline LuneValue lune_value_null(void) {
    return (LuneValue){.kind = LUNE_VALUE_NULL};
}

static inline LuneValue lune_value_bool(bool value) {
    return (LuneValue){.kind = LUNE_VALUE_BOOL, .as.boolean = value};
}

static inline LuneValue lune_value_int(int64_t value) {
    return (LuneValue){.kind = LUNE_VALUE_INT, .as.integer = value};
}

static inline LuneValue lune_value_float(double value) {
    return (LuneValue){.kind = LUNE_VALUE_FLOAT, .as.floating = value};
}

static inline LuneValue lune_value_obj(LuneObj *object) {
    return (LuneValue){.kind = LUNE_VALUE_OBJ, .as.object = object};
}

static inline bool lune_value_truthy(LuneValue value) {
    if (value.kind == LUNE_VALUE_NULL) return false;
    if (value.kind == LUNE_VALUE_BOOL) return value.as.boolean;
    return true;
}

bool lune_value_equal(LuneValue a, LuneValue b);
void lune_value_print(FILE *out, LuneValue value);

#endif
