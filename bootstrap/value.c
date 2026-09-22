#include "value.h"

#include <inttypes.h>

LuneValue lune_value_null(void) { return (LuneValue){.kind=LUNE_VALUE_NULL}; }
LuneValue lune_value_bool(bool value) { return (LuneValue){.kind=LUNE_VALUE_BOOL,.as.boolean=value}; }
LuneValue lune_value_int(int64_t value) { return (LuneValue){.kind=LUNE_VALUE_INT,.as.integer=value}; }
LuneValue lune_value_float(double value) { return (LuneValue){.kind=LUNE_VALUE_FLOAT,.as.floating=value}; }

bool lune_value_truthy(LuneValue value) {
    if (value.kind == LUNE_VALUE_NULL) return false;
    if (value.kind == LUNE_VALUE_BOOL) return value.as.boolean;
    return true;
}

bool lune_value_equal(LuneValue a, LuneValue b) {
    if (a.kind == LUNE_VALUE_INT && b.kind == LUNE_VALUE_FLOAT)
        return (double)a.as.integer == b.as.floating;
    if (a.kind == LUNE_VALUE_FLOAT && b.kind == LUNE_VALUE_INT)
        return a.as.floating == (double)b.as.integer;
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case LUNE_VALUE_NULL: return true;
        case LUNE_VALUE_BOOL: return a.as.boolean == b.as.boolean;
        case LUNE_VALUE_INT: return a.as.integer == b.as.integer;
        case LUNE_VALUE_FLOAT: return a.as.floating == b.as.floating;
    }
    return false;
}

void lune_value_print(FILE *out, LuneValue value) {
    switch (value.kind) {
        case LUNE_VALUE_NULL: fputs("null", out); break;
        case LUNE_VALUE_BOOL: fputs(value.as.boolean ? "true" : "false", out); break;
        case LUNE_VALUE_INT: fprintf(out, "%" PRId64, value.as.integer); break;
        case LUNE_VALUE_FLOAT: fprintf(out, "%.17g", value.as.floating); break;
    }
}
