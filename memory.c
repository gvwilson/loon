#include <stdlib.h>
#include <stdio.h>

#include "compiler.h"
#include "config.h"
#include "constants.h"
#include "debug.h"
#include "memory.h"
#include "native.h"
#include "table.h"
#include "vm.h"

#define GC_HEAP_GROW_FACTOR 2

void* reallocate(void* pointer, size_t oldSize, size_t newSize) {
  vm_.bytesAllocated += newSize - oldSize;
  if (newSize > oldSize) {
    if (vm_.bytesAllocated > vm_.nextGC) {
      collectGarbage();
    }
  }

  if (newSize == 0) {
    free(pointer);
    return NULL;
  }

  void* result = realloc(pointer, newSize);
  if (result == NULL) {
    exit(1);
  }
  return result;
}

void markObject(Obj* object) {
  if ((object == NULL) || (object->isMarked)) {
    return;
  }

  if (config_.dbg_gc) {
    print("%p mark ", (void*)object);
    printValue(OBJ_VAL(object));
    print("\n");
  }

  object->isMarked = true;

  if (vm_.grayCapacity < vm_.grayCount + 1) {
    vm_.grayCapacity = GROW_CAPACITY(vm_.grayCapacity);
    vm_.grayStack = (Obj**)realloc(vm_.grayStack, sizeof(Obj*) * vm_.grayCapacity);

    if (vm_.grayStack == NULL) {
      exit(1);
    }
  }

  vm_.grayStack[vm_.grayCount++] = object;
}

void markValue(Value value) {
  if (IS_OBJ(value)) {
    markObject(AS_OBJ(value));
  }
}

void markArray(ValueArray* array) {
  for (int i = 0; i < array->count; i++) {
    markValue(array->values[i]);
  }
}

static void markFiber(ObjFiber* fiber) {
  if (fiber == NULL) {
    return;
  }
  for (Value* slot = fiber->stack; slot < fiber->stackTop; slot++) {
    markValue(*slot);
  }

  for (int i = 0; i < fiber->frameCount; i++) {
    markObject((Obj*)fiber->frames[i].closure);
  }

  for (ObjUpvalue* upvalue = fiber->openUpvalues; upvalue != NULL; upvalue = upvalue->next) {
    markObject((Obj*)upvalue);
  }
}

static void blackenObject(Obj* object) {
  if (config_.dbg_gc) {
    print("%p blacken ", (void*)object);
    printValue(OBJ_VAL(object));
    print("\n");
  }

  switch (object->type) {
    case OBJ_BOUND_METHOD: {
      ObjBoundMethod* bound = (ObjBoundMethod*)object;
      markValue(bound->receiver);
      markObject((Obj*)bound->method);
      break;
    }

    case OBJ_CLASS: {
      ObjClass* klass = (ObjClass*)object;
      markObject((Obj*)klass->name);
      markTable(&klass->methods);
      break;
    }

    case OBJ_CLOSURE: {
      ObjClosure* closure = (ObjClosure*)object;
      markObject((Obj*)closure->function);
      for (int i = 0; i < closure->upvalueCount; i++) {
        markObject((Obj*)closure->upvalues[i]);
      }
      break;
    }

    case OBJ_FIBER: {
      markFiber((ObjFiber*)object);
      break;
    }

    case OBJ_FUNCTION: {
      ObjFunction* function = (ObjFunction*)object;
      markObject((Obj*)function->name);
      markArray(&function->chunk.constants);
      break;
    }

    case OBJ_INSTANCE: {
      ObjInstance* instance = (ObjInstance*)object;
      markObject((Obj*)instance->klass);
      markTable(&instance->fields);
      break;
    }

    case OBJ_UPVALUE: {
      markValue(((ObjUpvalue*)object)->closed);
      break;
    }

    case OBJ_LIST: {
      ObjList* list = (ObjList*)object;
      markArray(&list->values);
      break;
    }

    case OBJ_TABLE: {
      ObjTable* table = (ObjTable*)object;
      markTable(&table->values);
      break;
    }

    case OBJ_NATIVE:
    case OBJ_STRING:
      break;
  }
}

void freeObject(Obj* object) {
  if (config_.dbg_gc) {
    print("%p free type %d %s\n", (void*)object, object->type, objectTypeName(object->type));
  }

  switch (object->type) {
    case OBJ_BOUND_METHOD: {
      FREE(ObjBoundMethod, object);
      break;
    }
    case OBJ_CLASS: {
      ObjClass* klass = (ObjClass*)object;
      freeTable(&klass->methods);
      FREE(ObjClass, object);
      break;
    }
    case OBJ_CLOSURE: {
      ObjClosure* closure = (ObjClosure*)object;
      FREE_ARRAY(ObjUpvalue*, closure->upvalues, closure->upvalueCount);
      FREE(ObjClosure, object);
      break;
    }
    case OBJ_FIBER: {
      // FIXME: memory leak?
      FREE(ObjFiber, object);
      break;
    }
    case OBJ_FUNCTION: {
      ObjFunction* function = (ObjFunction*)object;
      freeChunk(&function->chunk);
      FREE(ObjFunction, object);
      break;
    }
    case OBJ_INSTANCE: {
      ObjInstance* instance = (ObjInstance*)object;
      freeTable(&instance->fields);
      FREE(ObjInstance, object);
      break;
    }
    case OBJ_NATIVE: {
      FREE(ObjNative, object);
      break;
    }
    case OBJ_STRING: {
      ObjString* string = (ObjString*)object;
      FREE_ARRAY(char, string->chars, string->length + 1);
      FREE(ObjString, object);
      break;
    }
    case OBJ_LIST: {
      ObjList* list = (ObjList*)object;
      freeValueArray(&list->values);
      FREE(ObjList, object);
      break;
    }
    case OBJ_TABLE: {
      ObjTable* table = (ObjTable*)object;
      freeTable(&table->values);
      FREE(ObjTable, object);
      break;
    }
    case OBJ_UPVALUE:
      FREE(ObjUpvalue, object);
      break;
  }
}

static void markRoots() {
  markFiber(vm_.current);
  markTable(&vm_.globals);
  markCompilerRoots();
  markConstants();
}

static void traceReferences() {
  while (vm_.grayCount > 0) {
    Obj* object = vm_.grayStack[--vm_.grayCount];
    blackenObject(object);
  }
}

static void sweep() {
  Obj* previous = NULL;
  Obj* object = vm_.objects;
  while (object != NULL) {
    if (object->isMarked) {
      object->isMarked = false;
      previous = object;
      object = object->next;
    }
    else {
      Obj* unreached = object;
      object = object->next;
      if (previous != NULL) {
        previous->next = object;
      }
      else {
        vm_.objects = object;
      }

      freeObject(unreached);
    }
  }
}

int collectGarbage() {
  size_t before = vm_.bytesAllocated;
  if (config_.dbg_gc) {
    print("-- gc begin\n");
  }

  markRoots();
  traceReferences();
  tableRemoveWhite(&vm_.strings);
  sweep();

  vm_.nextGC = vm_.bytesAllocated * GC_HEAP_GROW_FACTOR;

  int collected = before - vm_.bytesAllocated;
  if (config_.dbg_gc) {
    print("-- gc end\n");
    print("   collected %zu bytes (from %zu to %zu) next at %zu\n",
	  collected, before, vm_.bytesAllocated, vm_.nextGC);
  }
  return collected;
}

void freeObjects() {
  Obj* object = vm_.objects;
  while (object != NULL) {
    Obj* next = object->next;
    freeObject(object);
    object = next;
  }

  free(vm_.grayStack);
}
