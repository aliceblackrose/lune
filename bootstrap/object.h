#ifndef LUNE_OBJECT_H
#define LUNE_OBJECT_H

#include "value.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef enum {
    LUNE_OBJ_STRING,
    LUNE_OBJ_LIST,
    LUNE_OBJ_MAP
} LuneObjKind;

struct LuneObj {
    LuneObjKind kind;
    LuneObj *next;
};

typedef struct {
    LuneObj obj;
    size_t length;
    char *chars;
} LuneObjString;

typedef struct {
    LuneObj obj;
    size_t count;
    LuneValue *items;
} LuneObjList;

typedef struct {
    LuneObjString *key;
    LuneValue value;
} LuneMapValue;

typedef struct {
    LuneObj obj;
    size_t count;
    size_t capacity;
    LuneMapValue *entries;
} LuneObjMap;

typedef struct {
    LuneObj *objects;
} LuneHeap;

void lune_heap_init(LuneHeap *heap);
void lune_heap_free(LuneHeap *heap);

LuneObjString *lune_string_new(LuneHeap *heap, const char *chars, size_t length);
LuneObjString *lune_string_concat(
    LuneHeap *heap,
    const LuneObjString *a,
    const LuneObjString *b
);
LuneObjList *lune_list_new(LuneHeap *heap, const LuneValue *items, size_t count);
LuneObjMap *lune_map_new(LuneHeap *heap);

bool lune_map_get(
    const LuneObjMap *map,
    const LuneObjString *key,
    LuneValue *value
);
bool lune_map_get_chars(
    const LuneObjMap *map,
    const char *chars,
    size_t length,
    LuneValue *value
);
bool lune_map_set(
    LuneHeap *heap,
    LuneObjMap *map,
    LuneObjString *key,
    LuneValue value
);
bool lune_map_set_chars(
    LuneHeap *heap,
    LuneObjMap *map,
    const char *chars,
    size_t length,
    LuneValue value
);

bool lune_obj_is_string(const LuneObj *object);
bool lune_obj_is_list(const LuneObj *object);
bool lune_obj_is_map(const LuneObj *object);
void lune_object_print(FILE *out, const LuneObj *object);

#endif
