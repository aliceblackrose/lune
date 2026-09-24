#include "value.h"

#include "object.h"

#include <inttypes.h>
#include <string.h>

bool lune_value_equal(LuneValue a, LuneValue b) {
    if (a.kind == LUNE_VALUE_INT && b.kind == LUNE_VALUE_FLOAT)
        return (double)a.as.integer == b.as.floating;
    if (a.kind == LUNE_VALUE_FLOAT && b.kind == LUNE_VALUE_INT)
        return a.as.floating == (double)b.as.integer;
    if (a.kind != b.kind) return false;

    switch (a.kind) {
        case LUNE_VALUE_NULL:
            return true;
        case LUNE_VALUE_BOOL:
            return a.as.boolean == b.as.boolean;
        case LUNE_VALUE_INT:
            return a.as.integer == b.as.integer;
        case LUNE_VALUE_FLOAT:
            return a.as.floating == b.as.floating;
        case LUNE_VALUE_OBJ:
            if (a.as.object == b.as.object) return true;
            if (lune_obj_is_string(a.as.object) &&
                lune_obj_is_string(b.as.object)) {
                const LuneObjString *left = (const LuneObjString *)a.as.object;
                const LuneObjString *right = (const LuneObjString *)b.as.object;
                return left->length == right->length &&
                    memcmp(left->chars, right->chars, left->length) == 0;
            }
            return false;
    }
    return false;
}

void lune_value_print(FILE *out, LuneValue value) {
    switch (value.kind) {
        case LUNE_VALUE_NULL:
            fputs("null", out);
            break;
        case LUNE_VALUE_BOOL:
            fputs(value.as.boolean ? "true" : "false", out);
            break;
        case LUNE_VALUE_INT:
            fprintf(out, "%" PRId64, value.as.integer);
            break;
        case LUNE_VALUE_FLOAT:
            fprintf(out, "%.17g", value.as.floating);
            break;
        case LUNE_VALUE_OBJ:
            lune_object_print(out, value.as.object);
            break;
    }
}
