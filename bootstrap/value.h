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

LuneValue lune_value_null(void);
LuneValue lune_value_bool(bool value);
LuneValue lune_value_int(int64_t value);
LuneValue lune_value_float(double value);
LuneValue lune_value_obj(LuneObj *object);
bool lune_value_truthy(LuneValue value);
bool lune_value_equal(LuneValue a, LuneValue b);
void lune_value_print(FILE *out, LuneValue value);

#endif
