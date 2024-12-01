#include <time.h>

#include "common.h"
#include "constants.h"
#include "debug.h"
#include "memory.h"
#include "native.h"
#include "string.h"
#include "vm.h"

static void defineNative(const char* name, NativeFn function) {
  push(OBJ_VAL(copyString(name, (int)strlen(name))));
  push(OBJ_VAL(newNative(function)));
  tableSet(&vm_.globals, AS_STRING(vm_.current->stack[0]), vm_.current->stack[1]);
  pop();
  pop();
}

// ----------------------------------------------------------------------

static Value _concat_(int argc, Value* argv) {
  ObjString* a = AS_STRING(argv[0]);
  ObjString* b = AS_STRING(argv[1]);

  int length = a->length + b->length;
  char* chars = ALLOCATE(char, length + 1);
  memcpy(chars, a->chars, a->length);
  memcpy(chars + a->length, b->chars, b->length);
  chars[length] = '\0';

  ObjString* result = takeString(chars, length);
  return OBJ_VAL(result);
}

static Value _clock_(int argc, Value* argv) {
  return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

static Value _gc_(int argc, Value* argv) {
  return NUMBER_VAL(collectGarbage());
}

static Value _globals_(int argc, Value* argv) {
  printTable(&vm_.globals);
  return NIL_VAL;
}

static Value _has_(int argc, Value* argv) {
  // FIXME: check that there are two arguments
  // FIXME: check that the first is an instance or class
  // FIXME: check that the second is a string

  bool has = false;
  Value temp;

  if (IS_CLASS(argv[0])) {
    ObjClass* klass = AS_CLASS(argv[0]);
    ObjString* name = AS_STRING(argv[1]);
    has = tableGet(&klass->methods, name, &temp);
  }
  else if (IS_INSTANCE(argv[0])) {
    ObjInstance* instance = AS_INSTANCE(argv[0]);
    ObjString* name = AS_STRING(argv[1]);
    has = tableGet(&instance->fields, name, &temp) ||
          tableGet(&instance->klass->methods, name, &temp);
  }
  return BOOL_VAL(has);
}

static Value _str_(int argc, Value* argv) {
  // FIXME: check that there's just one
  return valueToString(argv[0]);
}

static Value _objects_(int argc, Value* argv) {
  printAllObjects();
  return NIL_VAL;
}

static Value _print_(int argc, Value* argv) {
  printValue(argv[0]);
  print("\n");
  return NIL_VAL;
}

static Value _type_(int argc, Value* argv) {
  Value value = argv[0];
  if (IS_BOOL(value)) {
    return strBool_;
  }
  else if (IS_NIL(value)) {
    return strNil_;
  }
  else if (IS_NUMBER(value)) {
    return strNumber_;
  }
  else if (IS_BOUND_METHOD(value)) {
    return strBoundMethod_;
  }
  else if (IS_CLASS(value)) {
    return strClass_;
  }
  else if (IS_CLOSURE(value) || IS_FUNCTION(value)) {
    return strFunction_;
  }
  else if (IS_INSTANCE(value)) {
    return strInstance_;
  }
  else if (IS_NATIVE(value)) {
    return strNative_;
  }
  else if (IS_STRING(value)) {
    return strString_;
  }
  else if (IS_LIST(value)) {
    return strList_;
  }
  else if (IS_TABLE(value)) {
    return strTable_;
  }
  else {
    return strUnknown_;
  }
}

void initCoreMisc() {
  defineNative("_concat_", _concat_);
  defineNative("clock", _clock_);
  defineNative("gc", _gc_);
  defineNative("globals", _globals_);
  defineNative("has", _has_);
  defineNative("_str_", _str_);
  defineNative("objects", _objects_);
  defineNative("print", _print_);
  defineNative("type", _type_);
}

// ----------------------------------------------------------------------

void printCoreList(ObjList* list) {
  print("[");
  for (int i=0; i<list->values.count; i++) {
    if (i > 0) {
      print(", ");
    }
    printValue(list->values.values[i]);
  }
  print("]");
}

static Value _list_add_(int argc, Value* argv) {
  // FIXME: check that there are two values
  // FIXME: check that the first is a list
  ObjList* list = (ObjList*)AS_OBJ(argv[0]);
  Value value = argv[1];
  writeValueArray(&list->values, value);
  return NUMBER_VAL(list->values.count - 1);
}

static Value _list_del_(int argc, Value* argv) {
  // FIXME: check that there are two values
  // FIXME: check that the first is a list
  ObjList* list = (ObjList*)AS_OBJ(argv[0]);
  int index = AS_NUMBER(argv[1]);
  if ((0 <= index) && (index < list->values.count)) {
    for (int i=index; i<list->values.count-1; i++) {
      list->values.values[i] = list->values.values[i+1];
    }
    list->values.count--;
    list->values.values[list->values.count] = NIL_VAL;
  }
  return NIL_VAL;
}

static Value _list_get_(int argc, Value* argv) {
  // FIXME: check that there are two values
  // FIXME: check that the first is a list
  // FIXME: check that the second is a legal index
  ObjList* list = (ObjList*)AS_OBJ(argv[0]);
  int index = AS_NUMBER(argv[1]);
  return list->values.values[index];
}

static Value _list_insert_(int argc, Value* argv) {
  // FIXME: check that there are three values
  // FIXME: check that the first is a list
  // FIXME: check that the second is a legal index
  ObjList* list = (ObjList*)AS_OBJ(argv[0]);
  int index = AS_NUMBER(argv[1]);
  Value value = argv[2];

  // Inserting into an empty list.
  if (list->values.count == 0) {
    if (index == 0) {
      writeValueArray(&list->values, value);
    }
  }

  // Inserting at the end.
  else if (index == list->values.count) {
    writeValueArray(&list->values, value);
  }

  // Inserting in range.
  else if ((0 <= index) && (index < list->values.count)) {
    // Add a value to the end of the array to increase the size.
    writeValueArray(&list->values, NIL_VAL);

    // Copy values up.
    for (int i=list->values.count-1; i>index; i--) {
      list->values.values[i] = list->values.values[i-1];
    }

    // Insert.
    list->values.values[index] = value;
  }

  return NIL_VAL;
}

static Value _list_len_(int argc, Value* argv) {
  // FIXME: check that there is just one value
  // FIXME: check that the value is a list
  ObjList* list = (ObjList*)AS_OBJ(argv[0]);
  return NUMBER_VAL(list->values.count);
}

static Value _list_new_(int argc, Value* argv) {
  // FIXME: check that there are no arguments
  ObjList* list = newCoreList();
  return OBJ_VAL(list);
}

static Value _list_set_(int argc, Value* argv) {
  // FIXME: check that there are three values
  // FIXME: check that the first is a list
  // FIXME: check that the second is a legal index
  ObjList* list = (ObjList*)AS_OBJ(argv[0]);
  int index = AS_NUMBER(argv[1]);
  Value value = argv[2];
  list->values.values[index] = value;
  return NIL_VAL;
}

static Value _list_str_(int argc, Value* argv) {
  return valueToString(argv[0]);
}

void initCoreList() {
  defineNative("_list_add_", _list_add_);
  defineNative("_list_del_", _list_del_);
  defineNative("_list_get_", _list_get_);
  defineNative("_list_insert_", _list_insert_);
  defineNative("_list_len_", _list_len_);
  defineNative("_list_new_", _list_new_);
  defineNative("_list_set_", _list_set_);
  defineNative("_list_str_", _list_str_);
}

// ----------------------------------------------------------------------

void printCoreTable(ObjTable* table) {
  printTable(&table->values);
}

static Value _table_del_(int argc, Value* argv) {
  // FIXME: check that there are two values
  // FIXME: check that the first is a table
  // FIXME: check that the second is a string
  ObjTable* table = (ObjTable*)AS_OBJ(argv[0]);
  ObjString* key = AS_STRING(argv[1]);
  tableDelete(&table->values, key);
  return NIL_VAL;
}

static Value _table_get_(int argc, Value* argv) {
  // FIXME: check that there are two values
  // FIXME: check that the first is a table
  // FIXME: check that the second is a string
  ObjTable* table = (ObjTable*)AS_OBJ(argv[0]);
  ObjString* key = AS_STRING(argv[1]);
  Value value;
  if (tableGet(&table->values, key, &value)) {
    return value;
  }
  return NIL_VAL;
}

static Value _table_len_(int argc, Value* argv) {
  // FIXME: check that there is just one value
  // FIXME: check that the value is a table
  ObjTable* table = (ObjTable*)AS_OBJ(argv[0]);
  return NUMBER_VAL(countTableLive(&table->values));
}

static Value _table_new_(int argc, Value* argv) {
  // FIXME: check that there are no arguments
  ObjTable* table = newCoreTable();
  return OBJ_VAL(table);
}

static Value _table_set_(int argc, Value* argv) {
  // FIXME: check that there are three values
  // FIXME: check that the first is a table
  // FIXME: check that the second is a string
  ObjTable* table = (ObjTable*)AS_OBJ(argv[0]);
  ObjString* key = AS_STRING(argv[1]);
  Value value = argv[2];
  tableSet(&table->values, key, value);
  return NIL_VAL;
}

static Value _table_str_(int argc, Value* argv) {
  return valueToString(argv[0]);
}

void initCoreTable() {
  defineNative("_tbl_del_", _table_del_);
  defineNative("_tbl_get_", _table_get_);
  defineNative("_tbl_len_", _table_len_);
  defineNative("_tbl_new_", _table_new_);
  defineNative("_tbl_set_", _table_set_);
  defineNative("_tbl_str_", _table_str_);
}

// ----------------------------------------------------------------------

static Value _fiber_new_(int argc, Value* argv) {
  ObjFiber* fiber = newFiber(vm_.current);
  return OBJ_VAL(fiber);
}

static Value _fiber_run_(int argc, Value* argv) {
  ObjFiber* fiber = AS_FIBER(argv[0]);
  // FIXME: switch
  return NIL_VAL; // FIXME
}

static Value _fiber_yield_(int argc, Value* argv) {
  return NIL_VAL; // FIXME
}

void initCoreFiber() {
  defineNative("_fiber_new_", _fiber_new_);
  defineNative("_fiber_run_", _fiber_run_);
  defineNative("yield", _fiber_yield_);
}

// ----------------------------------------------------------------------

void initNative() {
  initCoreMisc();
  initCoreList();
  initCoreTable();
  initCoreFiber();
}
