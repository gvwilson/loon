#include <stdio.h>

#include "constants.h"
#include "memory.h"
#include "object.h"
#include "string.h"
#include "table.h"

#define MAX_NUM_VALUES 10

static const char* strItemSep_ = ", ";
static const int strItemSepLen_ = 2;
static const char* strEntrySep_ = ": ";
static const int strEntrySepLen_ = 2;

static Value objectToString(Value value);

static Value functionToString(ObjFunction* function) {
  if (function->name == NULL) {
    return strScript_;
  }
  char* s = NULL;
  int len = asprintf(&s, "<fn %.*s>", function->name->length, function->name->chars);
  return OBJ_VAL(takeString(s, len));
}

static Value listToString(ObjList* list) {
  // Setup.
  int numValues = list->values.count;
  if (numValues > MAX_NUM_VALUES) {
    numValues = MAX_NUM_VALUES;
  }

  // Convert elements.
  int totalLen = 0;
  Value values[MAX_NUM_VALUES];
  for (int i=0; i<numValues; i++) {
    values[i] = valueToString(list->values.values[i]);
    totalLen += AS_STRING(values[i])->length + strItemSepLen_;
  }

  // Concatenate.
  totalLen += 2; // '['...']'
  char* buffer = ALLOCATE(char, totalLen+1);
  char* current = buffer;
  current += sprintf(current, "[");
  for (int i=0; i<numValues; i++) {
    if (i > 0) {
      current += sprintf(current, "%s", strItemSep_);
    }
    ObjString* s = AS_STRING(values[i]);
    current += sprintf(current, "%.*s", s->length, s->chars);
  }
  current += sprintf(current, "]");

  return OBJ_VAL(takeString(buffer, totalLen));
}

static Value tableToString(ObjTable* table) {
  // Setup.
  int numValues = countTableLive(&table->values);
  if (numValues > MAX_NUM_VALUES) {
    numValues = MAX_NUM_VALUES;
  }

  // Convert elements.
  int loc = 0;
  int totalLen = 0;
  Value values[2 * MAX_NUM_VALUES]; // key and value
  for (int i=0; i<table->values.capacity; i++) {
    ObjString* key = table->values.entries[i].key;
    Value value = table->values.entries[i].value;
    if (key != NULL) {
      values[loc] = OBJ_VAL(key);
      totalLen += AS_STRING(values[loc])->length + strEntrySepLen_;
      values[loc+1] = valueToString(value);
      totalLen += AS_STRING(values[loc+1])->length + strItemSepLen_;
      loc += 2;
      if (loc == numValues) {
	break;
      }
    }
  }

  // Concatenate.
  totalLen += 2; // '{'...'}'
  char* buffer = ALLOCATE(char, totalLen+1);
  char* current = buffer;
  current += sprintf(current, "{");
  for (int i=0; i<numValues; i+=2) {
    if (i > 0) {
      current += sprintf(current, "%s", strItemSep_);
    }
    ObjString* s = AS_STRING(values[i]);
    current += sprintf(current, "%.*s", s->length, s->chars);
    current += sprintf(current, "%s", strEntrySep_);
    s = AS_STRING(values[i+1]);
    current += sprintf(current, "%.*s", s->length, s->chars);
  }
  current += sprintf(current, "}");

  return OBJ_VAL(takeString(buffer, totalLen));
}

static Value objectToString(Value value) {
  switch (OBJ_TYPE(value)) {
    case OBJ_BOUND_METHOD: {
      return functionToString(AS_BOUND_METHOD(value)->method->function);
    }
    case OBJ_CLASS: {
      return OBJ_VAL(AS_CLASS(value)->name);
    }
    case OBJ_CLOSURE: {
      return functionToString(AS_CLOSURE(value)->function);
    }
    case OBJ_FIBER: {
      char* s = NULL;
      ObjFiber* fiber = AS_FIBER(value);
      int len = asprintf(&s, "<fiber %d/%d>", fiber->id,
			 (fiber->parent == NULL) ? -1 : fiber->parent->id);
      return OBJ_VAL(takeString(s, len));
    }
    case OBJ_FUNCTION: {
      return functionToString(AS_FUNCTION(value));
    }
    case OBJ_INSTANCE: {
      ObjString* name = AS_INSTANCE(value)->klass->name;
      char* s = NULL;
      int len = asprintf(&s, "%.*s instance", name->length, name->chars);
      return OBJ_VAL(takeString(s, len));
    }
    case OBJ_LIST: {
      return listToString(AS_LIST(value));
    }
    case OBJ_NATIVE: {
      return strNativeFn_;
    }
    case OBJ_STRING: {
      return value;
    }
    case OBJ_TABLE: {
      return tableToString(AS_TABLE(value));
    }
    case OBJ_UPVALUE: {
      return strUpvalue_;
    }
    default: {
      return strUnknown_;
    }
  }
}

Value valueToString(Value value) {
  if (IS_BOOL(value)) {
    return AS_BOOL(value) ? strTrue_ : strFalse_;
  }
  else if (IS_NIL(value)) {
    return strNil_;
  }
  else if (IS_NUMBER(value)) {
    char* buffer = NULL;
    int len = asprintf(&buffer, "%g", AS_NUMBER(value));
    return OBJ_VAL(takeString(buffer, len));
  }
  else if (IS_OBJ(value)) {
    return objectToString(value);
  }
  else {
    return strUnknown_;
  }
}
