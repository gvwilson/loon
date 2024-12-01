#include <stdio.h>
#include <string.h>

#include "config.h"
#include "memory.h"
#include "object.h"
#include "table.h"
#include "value.h"
#include "vm.h"

#define ALLOCATE_OBJ(type, objectType) (type*)allocateObject(sizeof(type), objectType)

static const char* object_type_names[] = {
  [OBJ_BOUND_METHOD] = "bound method",
  [OBJ_CLASS] = "class",
  [OBJ_CLOSURE] = "closure",
  [OBJ_FUNCTION] = "function",
  [OBJ_INSTANCE] = "instance",
  [OBJ_NATIVE] = "native",
  [OBJ_STRING] = "string",
  [OBJ_UPVALUE] = "upvalue",
  [OBJ_LIST] = "list"
};

const char* objectTypeName(ObjType type) {
  return object_type_names[type];
}

static Obj* allocateObject(size_t size, ObjType type) {
  Obj* object = (Obj*)reallocate(NULL, 0, size);
  object->type = type;
  object->isMarked = false;

  object->next = vm_.objects;
  vm_.objects = object;

  if (config_.dbg_gc) {
    print("%p allocate %zu for %s\n", (void*)object, size, objectTypeName(type));
  }

  return object;
}

ObjBoundMethod* newBoundMethod(Value receiver, ObjClosure* method) {
  ObjBoundMethod* bound = ALLOCATE_OBJ(ObjBoundMethod, OBJ_BOUND_METHOD);
  bound->receiver = receiver;
  bound->method = method;
  return bound;
}

ObjClass* newClass(ObjString* name) {
  ObjClass* klass = ALLOCATE_OBJ(ObjClass, OBJ_CLASS);
  klass->name = name;
  initTable(&klass->methods);
  return klass;
}

ObjClosure* newClosure(ObjFunction* function) {
  ObjUpvalue** upvalues = ALLOCATE(ObjUpvalue*, function->upvalueCount);
  for (int i = 0; i < function->upvalueCount; i++) {
    upvalues[i] = NULL;
  }

  ObjClosure* closure = ALLOCATE_OBJ(ObjClosure, OBJ_CLOSURE);
  closure->function = function;
  closure->upvalues = upvalues;
  closure->upvalueCount = function->upvalueCount;
  return closure;
}

void resetStack(ObjFiber* fiber) {
  fiber->frameCount = 0;
  fiber->stackTop = fiber->stack;
  fiber->openUpvalues = NULL;
}

static int fiberId_ = 0;

ObjFiber* newFiber(ObjFiber* parent) {
  ObjFiber* fiber = ALLOCATE_OBJ(ObjFiber, OBJ_FIBER);
  fiber->id = fiberId_++;
  fiber->parent = parent;
  resetStack(fiber);
  return fiber;
}

ObjFunction* newFunction() {
  ObjFunction* function = ALLOCATE_OBJ(ObjFunction, OBJ_FUNCTION);
  function->arity = 0;
  function->upvalueCount = 0;
  function->name = NULL;
  initChunk(&function->chunk);
  return function;
}

ObjInstance* newInstance(ObjClass* klass) {
  ObjInstance* instance = ALLOCATE_OBJ(ObjInstance, OBJ_INSTANCE);
  instance->klass = klass;
  initTable(&instance->fields);
  return instance;
}

ObjNative* newNative(NativeFn function) {
  ObjNative* native = ALLOCATE_OBJ(ObjNative, OBJ_NATIVE);
  native->function = function;
  return native;
}

static ObjString* allocateString(char* chars, int length, uint32_t hash) {
  ObjString* string = ALLOCATE_OBJ(ObjString, OBJ_STRING);
  string->length = length;
  string->chars = chars;
  string->hash = hash;

  push(OBJ_VAL(string));
  tableSet(&vm_.strings, string, NIL_VAL);
  pop();

  return string;
}

static uint32_t hashString(const char* key, int length) {
  uint32_t hash = 2166136261u;
  for (int i = 0; i < length; i++) {
    hash ^= (Byte)key[i];
    hash *= 16777619;
  }
  return hash;
}

ObjString* takeString(char* chars, int length) {
  uint32_t hash = hashString(chars, length);
  ObjString* interned = tableFindString(&vm_.strings, chars, length, hash);
  if (interned != NULL) {
    FREE_ARRAY(char, chars, length + 1);
    return interned;
  }

  return allocateString(chars, length, hash);
}

ObjString* copyString(const char* chars, int length) {
  uint32_t hash = hashString(chars, length);
  ObjString* interned = tableFindString(&vm_.strings, chars, length, hash);
  if (interned != NULL) {
    return interned;
  }

  char* heapChars = ALLOCATE(char, length + 1);
  memcpy(heapChars, chars, length);
  heapChars[length] = '\0';
  return allocateString(heapChars, length, hash);
}

ObjUpvalue* newUpvalue(Value* slot) {
  ObjUpvalue* upvalue = ALLOCATE_OBJ(ObjUpvalue, OBJ_UPVALUE);
  upvalue->closed = NIL_VAL;
  upvalue->location = slot;
  upvalue->next = NULL;
  return upvalue;
}

ObjList* newCoreList() {
  ObjList* list = ALLOCATE_OBJ(ObjList, OBJ_LIST);
  initValueArray(&list->values);
  return list;
}

ObjTable* newCoreTable() {
  ObjTable* table = ALLOCATE_OBJ(ObjTable, OBJ_TABLE);
  initTable(&table->values);
  return table;
}
