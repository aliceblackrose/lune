#include "object.h"

#include "bytecode.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static LuneObj *allocate_object(
    LuneHeap *heap,
    size_t size,
    LuneObjKind kind
) {
    LuneObj *object = calloc(1, size);
    if (object == NULL) return NULL;

    object->kind = kind;
    object->next = heap->objects;
    heap->objects = object;
    return object;
}

void lune_heap_init(LuneHeap *heap) {
    heap->objects = NULL;
}

void lune_heap_free(LuneHeap *heap) {
    LuneObj *object = heap->objects;
    while (object != NULL) {
        LuneObj *next = object->next;

        switch (object->kind) {
            case LUNE_OBJ_STRING:
                free(((LuneObjString *)object)->chars);
                break;
            case LUNE_OBJ_LIST:
                free(((LuneObjList *)object)->items);
                break;
            case LUNE_OBJ_MAP:
                free(((LuneObjMap *)object)->entries);
                break;
            case LUNE_OBJ_CLOSURE:
                free(((LuneObjClosure *)object)->upvalues);
                break;
            case LUNE_OBJ_UPVALUE:
            case LUNE_OBJ_NATIVE:
                break;
        }

        free(object);
        object = next;
    }
    heap->objects = NULL;
}

LuneObjString *lune_string_new(
    LuneHeap *heap,
    const char *chars,
    size_t length
) {
    LuneObjString *string = (LuneObjString *)allocate_object(
        heap, sizeof(*string), LUNE_OBJ_STRING
    );
    if (string == NULL) return NULL;

    string->chars = malloc(length + 1);
    if (string->chars == NULL) return NULL;

    memcpy(string->chars, chars, length);
    string->chars[length] = '\0';
    string->length = length;
    return string;
}

LuneObjString *lune_string_concat(
    LuneHeap *heap,
    const LuneObjString *a,
    const LuneObjString *b
) {
    if (a->length > SIZE_MAX - b->length) return NULL;

    size_t length = a->length + b->length;
    char *chars = malloc(length + 1);
    if (chars == NULL) return NULL;

    memcpy(chars, a->chars, a->length);
    memcpy(chars + a->length, b->chars, b->length);
    chars[length] = '\0';

    LuneObjString *string = (LuneObjString *)allocate_object(
        heap, sizeof(*string), LUNE_OBJ_STRING
    );
    if (string == NULL) {
        free(chars);
        return NULL;
    }

    string->chars = chars;
    string->length = length;
    return string;
}

LuneObjList *lune_list_new(
    LuneHeap *heap,
    const LuneValue *items,
    size_t count
) {
    LuneObjList *list = (LuneObjList *)allocate_object(
        heap, sizeof(*list), LUNE_OBJ_LIST
    );
    if (list == NULL) return NULL;

    if (count > 0) {
        if (count > SIZE_MAX / sizeof(*list->items)) {
            return NULL;
        }

        list->items = malloc(count * sizeof(*list->items));
        if (list->items == NULL) return NULL;

        memcpy(
            list->items,
            items,
            count * sizeof(*list->items)
        );
    }

    list->count = count;
    return list;
}

LuneObjMap *lune_map_new(LuneHeap *heap) {
    return (LuneObjMap *)allocate_object(
        heap, sizeof(LuneObjMap), LUNE_OBJ_MAP
    );
}

static bool string_equal_chars(
    const LuneObjString *string,
    const char *chars,
    size_t length
) {
    return string->length == length &&
        memcmp(string->chars, chars, length) == 0;
}

bool lune_map_get_chars(
    const LuneObjMap *map,
    const char *chars,
    size_t length,
    LuneValue *value
) {
    for (size_t i = 0; i < map->count; i++) {
        if (string_equal_chars(
            map->entries[i].key, chars, length
        )) {
            *value = map->entries[i].value;
            return true;
        }
    }
    return false;
}

bool lune_map_get(
    const LuneObjMap *map,
    const LuneObjString *key,
    LuneValue *value
) {
    return lune_map_get_chars(
        map, key->chars, key->length, value
    );
}

static bool map_append(
    LuneObjMap *map,
    LuneObjString *key,
    LuneValue value
) {
    if (map->count == map->capacity) {
        size_t next = map->capacity == 0
            ? 8
            : map->capacity * 2;

        if (
            next < map->capacity ||
            next > SIZE_MAX / sizeof(*map->entries)
        ) {
            return false;
        }

        LuneMapValue *grown = realloc(
            map->entries, next * sizeof(*map->entries)
        );
        if (grown == NULL) return false;

        map->entries = grown;
        map->capacity = next;
    }

    map->entries[map->count++] = (LuneMapValue){
        .key = key,
        .value = value,
    };
    return true;
}

bool lune_map_set(
    LuneHeap *heap,
    LuneObjMap *map,
    LuneObjString *key,
    LuneValue value
) {
    (void)heap;

    for (size_t i = 0; i < map->count; i++) {
        if (string_equal_chars(
            map->entries[i].key,
            key->chars,
            key->length
        )) {
            map->entries[i].value = value;
            return true;
        }
    }

    return map_append(map, key, value);
}

bool lune_map_set_chars(
    LuneHeap *heap,
    LuneObjMap *map,
    const char *chars,
    size_t length,
    LuneValue value
) {
    for (size_t i = 0; i < map->count; i++) {
        if (string_equal_chars(
            map->entries[i].key, chars, length
        )) {
            map->entries[i].value = value;
            return true;
        }
    }

    LuneObjString *key = lune_string_new(
        heap, chars, length
    );
    if (key == NULL) return false;

    return map_append(map, key, value);
}

LuneObjClosure *lune_closure_new(
    LuneHeap *heap,
    const LuneFunction *function,
    size_t upvalue_count
) {
    LuneObjClosure *closure =
        (LuneObjClosure *)allocate_object(
            heap, sizeof(*closure), LUNE_OBJ_CLOSURE
        );
    if (closure == NULL) return NULL;

    if (upvalue_count > 0) {
        if (
            upvalue_count >
            SIZE_MAX / sizeof(*closure->upvalues)
        ) {
            return NULL;
        }

        closure->upvalues = calloc(
            upvalue_count, sizeof(*closure->upvalues)
        );
        if (closure->upvalues == NULL) return NULL;
    }

    closure->function = function;
    closure->upvalue_count = upvalue_count;
    return closure;
}

LuneObjUpvalue *lune_upvalue_new(
    LuneHeap *heap,
    LuneValue *location
) {
    LuneObjUpvalue *upvalue =
        (LuneObjUpvalue *)allocate_object(
            heap, sizeof(*upvalue), LUNE_OBJ_UPVALUE
        );
    if (upvalue == NULL) return NULL;

    upvalue->location = location;
    upvalue->closed = lune_value_null();
    return upvalue;
}

LuneObjNative *lune_native_new(
    LuneHeap *heap,
    const char *name,
    int arity,
    LuneNativeFn function
) {
    LuneObjNative *native =
        (LuneObjNative *)allocate_object(
            heap, sizeof(*native), LUNE_OBJ_NATIVE
        );
    if (native == NULL) return NULL;

    native->name = name;
    native->arity = arity;
    native->function = function;
    return native;
}

bool lune_obj_is_string(const LuneObj *object) {
    return object != NULL &&
        object->kind == LUNE_OBJ_STRING;
}

bool lune_obj_is_list(const LuneObj *object) {
    return object != NULL &&
        object->kind == LUNE_OBJ_LIST;
}

bool lune_obj_is_map(const LuneObj *object) {
    return object != NULL &&
        object->kind == LUNE_OBJ_MAP;
}

bool lune_obj_is_closure(const LuneObj *object) {
    return object != NULL &&
        object->kind == LUNE_OBJ_CLOSURE;
}

bool lune_obj_is_native(const LuneObj *object) {
    return object != NULL &&
        object->kind == LUNE_OBJ_NATIVE;
}

static void print_object(
    FILE *out,
    const LuneObj *object,
    unsigned depth
);

static void print_value(
    FILE *out,
    LuneValue value,
    unsigned depth
) {
    switch (value.kind) {
        case LUNE_VALUE_NULL:
            fputs("null", out);
            break;
        case LUNE_VALUE_BOOL:
            fputs(
                value.as.boolean ? "true" : "false", out
            );
            break;
        case LUNE_VALUE_INT:
            fprintf(
                out, "%lld", (long long)value.as.integer
            );
            break;
        case LUNE_VALUE_FLOAT:
            fprintf(out, "%.17g", value.as.floating);
            break;
        case LUNE_VALUE_OBJ:
            print_object(out, value.as.object, depth);
            break;
    }
}

static void print_object(
    FILE *out,
    const LuneObj *object,
    unsigned depth
) {
    if (depth > 8) {
        fputs("...", out);
        return;
    }

    switch (object->kind) {
        case LUNE_OBJ_STRING: {
            const LuneObjString *string =
                (const LuneObjString *)object;
            fwrite(
                string->chars, 1, string->length, out
            );
            break;
        }

        case LUNE_OBJ_LIST: {
            const LuneObjList *list =
                (const LuneObjList *)object;
            fputc('[', out);
            for (size_t i = 0; i < list->count; i++) {
                if (i != 0) fputs(", ", out);
                print_value(
                    out, list->items[i], depth + 1
                );
            }
            fputc(']', out);
            break;
        }

        case LUNE_OBJ_MAP: {
            const LuneObjMap *map =
                (const LuneObjMap *)object;
            fputc('{', out);
            for (size_t i = 0; i < map->count; i++) {
                if (i != 0) fputs(", ", out);
                fwrite(
                    map->entries[i].key->chars,
                    1,
                    map->entries[i].key->length,
                    out
                );
                fputs(": ", out);
                print_value(
                    out,
                    map->entries[i].value,
                    depth + 1
                );
            }
            fputc('}', out);
            break;
        }

        case LUNE_OBJ_CLOSURE: {
            const LuneObjClosure *closure =
                (const LuneObjClosure *)object;
            fprintf(
                out,
                "<fn/%u>",
                (unsigned)closure->function->arity
            );
            break;
        }

        case LUNE_OBJ_UPVALUE:
            fputs("<upvalue>", out);
            break;

        case LUNE_OBJ_NATIVE: {
            const LuneObjNative *native =
                (const LuneObjNative *)object;
            fprintf(out, "<native %s>", native->name);
            break;
        }
    }
}

void lune_object_print(
    FILE *out,
    const LuneObj *object
) {
    print_object(out, object, 0);
}
