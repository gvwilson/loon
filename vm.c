#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "compiler.h"
#include "config.h"
#include "constants.h"
#include "core.loon.h"
#include "debug.h"
#include "memory.h"
#include "native.h"
#include "object.h"
#include "string.h"
#include "vm.h"

VM vm_;

static void runtimeError(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputs("\n", stderr);

  for (int i = vm_.current->frameCount - 1; i >= 0; i--) {
    CallFrame* frame = &vm_.current->frames[i];
    ObjFunction* function = frame->closure->function;
    size_t instruction = frame->ip - function->chunk.code - 1;
    fprintf(stderr, "[line %d] in ", function->chunk.lines[instruction]);
    if (function->name == NULL) {
      fprintf(stderr, "script\n");
    }
    else {
      fprintf(stderr, "%s()\n", function->name->chars);
    }
  }

  resetStack(vm_.current);
}

static void initLibrary() {
  quietPrint();
  char terminatedCore[core_loon_len + 1];
  strncpy(terminatedCore, (const char*)core_loon, core_loon_len);
  terminatedCore[core_loon_len] = '\0';
  interpret(terminatedCore);
  restorePrint();
}

void initVM() {
  vm_.current = newFiber(NULL);
  resetStack(vm_.current);
  vm_.objects = NULL;
  vm_.bytesAllocated = 0;
  vm_.nextGC = 1024 * 1024;

  vm_.grayCount = 0;
  vm_.grayCapacity = 0;
  vm_.grayStack = NULL;

  initTable(&vm_.globals);
  initTable(&vm_.strings);

  initConstants();
  initNative();
  initLibrary();
}

void freeVM() {
  freeTable(&vm_.globals);
  freeTable(&vm_.strings);
  freeObjects();
}

void push(Value value) {
  *vm_.current->stackTop = value;
  vm_.current->stackTop++;
}

Value pop() {
  vm_.current->stackTop--;
  return *vm_.current->stackTop;
}

static Value peek(int distance) {
  return vm_.current->stackTop[-1 - distance];
}

static bool call(ObjClosure* closure, int argCount) {
  if (argCount != closure->function->arity) {
    runtimeError("Expected %d arguments but got %d.", closure->function->arity, argCount);
    return false;
  }

  if (vm_.current->frameCount == FRAMES_MAX) {
    runtimeError("Stack overflow.");
    return false;
  }

  CallFrame* frame = &vm_.current->frames[vm_.current->frameCount++];
  frame->closure = closure;
  frame->ip = closure->function->chunk.code;
  frame->slots = vm_.current->stackTop - argCount - 1;
  return true;
}

static bool callValue(Value callee, int argCount) {
  if (IS_OBJ(callee)) {
    switch (OBJ_TYPE(callee)) {
      case OBJ_BOUND_METHOD: {
        ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
        vm_.current->stackTop[-argCount - 1] = bound->receiver;
        return call(bound->method, argCount);
      }
      case OBJ_CLASS: {
        ObjClass* klass = AS_CLASS(callee);
        vm_.current->stackTop[-argCount - 1] = OBJ_VAL(newInstance(klass));
        Value initializer;
        if (tableGet(&klass->methods, AS_STRING(strInit_), &initializer)) {
          return call(AS_CLOSURE(initializer), argCount);
        }
        else if (argCount != 0) {
          runtimeError("Expected 0 arguments but got %d.", argCount);
          return false;
        }
        return true;
      }
      case OBJ_CLOSURE:
        return call(AS_CLOSURE(callee), argCount);
      case OBJ_NATIVE: {
        NativeFn native = AS_NATIVE(callee);
        Value result = native(argCount, vm_.current->stackTop - argCount);
        vm_.current->stackTop -= argCount + 1;
        push(result);
        return true;
      }
      default:
        break; // Non-callable object type.
    }
  }
  return callValue(callee, argCount);
}

static bool callValuePostfix(Value callee, int argCount) {
  for (int i=0; i<argCount; i++) {
    vm_.current->stackTop[-1 - i] = vm_.current->stackTop[-1 -i -1];
  }
  vm_.current->stackTop[-1 -argCount] = callee;
  return callValue(callee, argCount);
}

static bool invokeFromClass(ObjClass* klass, ObjString* name, int argCount) {
  Value method;
  if (!tableGet(&klass->methods, name, &method)) {
    runtimeError("Undefined property '%s'.", name->chars);
    return false;
  }
  return call(AS_CLOSURE(method), argCount);
}

static bool invoke(ObjString* name, int argCount) {
  Value receiver = peek(argCount);

  if (!IS_INSTANCE(receiver)) {
    runtimeError("Only instances have methods.");
    return false;
  }

  ObjInstance* instance = AS_INSTANCE(receiver);

  Value value;
  if (tableGet(&instance->fields, name, &value)) {
    vm_.current->stackTop[-argCount - 1] = value;
    return callValue(value, argCount);
  }

  return invokeFromClass(instance->klass, name, argCount);
}

static bool bindMethod(ObjClass* klass, ObjString* name) {
  Value method;
  if (!tableGet(&klass->methods, name, &method)) {
    runtimeError("Undefined property '%s'.", name->chars);
    return false;
  }

  ObjBoundMethod* bound = newBoundMethod(peek(0), AS_CLOSURE(method));
  pop();
  push(OBJ_VAL(bound));
  return true;
}

static ObjUpvalue* captureUpvalue(Value* local) {
  ObjUpvalue* prevUpvalue = NULL;
  ObjUpvalue* upvalue = vm_.current->openUpvalues;
  while ((upvalue != NULL) && (upvalue->location > local)) {
    prevUpvalue = upvalue;
    upvalue = upvalue->next;
  }

  if ((upvalue != NULL) && (upvalue->location == local)) {
    return upvalue;
  }

  ObjUpvalue* createdUpvalue = newUpvalue(local);
  createdUpvalue->next = upvalue;

  if (prevUpvalue == NULL) {
    vm_.current->openUpvalues = createdUpvalue;
  }
  else {
    prevUpvalue->next = createdUpvalue;
  }

  return createdUpvalue;
}

static void closeUpvalues(Value* last) {
  while ((vm_.current->openUpvalues != NULL) && (vm_.current->openUpvalues->location >= last)) {
    ObjUpvalue* upvalue = vm_.current->openUpvalues;
    upvalue->closed = *upvalue->location;
    upvalue->location = &upvalue->closed;
    vm_.current->openUpvalues = upvalue->next;
  }
}

static void defineMethod(ObjString* name) {
  Value method = peek(0);
  ObjClass* klass = AS_CLASS(peek(1));
  tableSet(&klass->methods, name, method);
  pop();
}

static bool isFalsey(Value value) {
  return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static inline Byte readByte(CallFrame** framePtr) {
  return *(*framePtr)->ip++;
}

static InterpretResult createCoreList(int numValues) {
  ObjList* list = newCoreList();
  for (int i=0; i<numValues; ++i) {
    writeValueArray(&list->values, pop());
  }
  reverseValueArray(&list->values);

  Value klass;
  if (!tableGet(&vm_.globals, AS_STRING(strListClass_), &klass)) {
    runtimeError("Cannot find definition of List class.");
    return INTERPRET_RUNTIME_ERROR;
  }

  ObjInstance* instance = newInstance(AS_CLASS(klass));
  tableSet(&instance->fields, AS_STRING(strData_), OBJ_VAL(list));
  push(OBJ_VAL(instance));

  return INTERPRET_OK;
}

static InterpretResult createCoreTable(int numValues) {
  ObjTable* table = newCoreTable();
  for (int i=0; i<numValues; i++) {
    Value value = pop();
    Value key = pop();
    if (!IS_STRING(key)) {
      runtimeError("Table keys must be strings.");
      return INTERPRET_RUNTIME_ERROR;
    }
    tableSet(&table->values, AS_STRING(key), value);
  }

  Value klass;
  if (!tableGet(&vm_.globals, AS_STRING(strTableClass_), &klass)) {
    runtimeError("Cannot find definition of Table class.");
    return INTERPRET_RUNTIME_ERROR;
  }

  ObjInstance* instance = newInstance(AS_CLASS(klass));
  tableSet(&instance->fields, AS_STRING(strData_), OBJ_VAL(table));
  push(OBJ_VAL(instance));

  return INTERPRET_OK;
}

static InterpretResult runSingle(ObjFiber** fiberPtr, CallFrame** framePtr, Byte instruction) {

#define READ_SHORT() \
  ((*framePtr)->ip += 2, \
  (uint16_t)(((*framePtr)->ip[-2] << BYTE_WIDTH) | (*framePtr)->ip[-1]))

#define READ_CONSTANT() \
  ((*framePtr)->closure->function->chunk.constants.values[readByte(framePtr)])

#define READ_STRING() AS_STRING(READ_CONSTANT())

#define BINARY_OP(valueType, op)                \
  do { \
    if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) { \
      runtimeError("Operands must be numbers."); \
      return INTERPRET_RUNTIME_ERROR; \
    } \
    double b = AS_NUMBER(pop()); \
    double a = AS_NUMBER(pop()); \
    push(valueType(a op b)); \
  } while (false)

  switch (instruction) {
    case OP_ADD: {
      BINARY_OP(NUMBER_VAL, +);
      break;
    }

    case OP_CALL: {
      int argCount = readByte(framePtr);
      if (!callValue(peek(argCount), argCount)) {
        return INTERPRET_RUNTIME_ERROR;
      }
      (*framePtr) = &vm_.current->frames[vm_.current->frameCount - 1];
      break;
    }

    case OP_CALL_POSTFIX: {
      int argCount = (int)readByte(framePtr);
      if (!callValuePostfix(peek(0), argCount)) {
	return INTERPRET_RUNTIME_ERROR;
      }
      (*framePtr) = &vm_.current->frames[vm_.current->frameCount - 1];
      break;
    }

    case OP_CLASS: {
      push(OBJ_VAL(newClass(READ_STRING())));
      break;
    }

    case OP_CLOSURE: {
      ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
      ObjClosure* closure = newClosure(function);
      push(OBJ_VAL(closure));
      for (int i = 0; i < closure->upvalueCount; i++) {
        Byte isLocal = readByte(framePtr);
        Byte index = readByte(framePtr);
        if (isLocal) {
          closure->upvalues[i] = captureUpvalue((*framePtr)->slots + index);
        }
	else {
          closure->upvalues[i] = (*framePtr)->closure->upvalues[index];
        }
      }
      break;
    }

    case OP_COLLECTION_LIST: {
      int numValues = readByte(framePtr);
      InterpretResult result = createCoreList(numValues);
      if (result != INTERPRET_OK) {
        return result;
      }
      break;
    }

    case OP_COLLECTION_TABLE: {
      int numValues = readByte(framePtr);
      InterpretResult result = createCoreTable(numValues);
      if (result != INTERPRET_OK) {
	return result;
      }
      break;
    }

    case OP_CONSTANT: {
      Value constant = READ_CONSTANT();
      push(constant);
      break;
    }

    case OP_DIVIDE: {
      BINARY_OP(NUMBER_VAL, /);
      break;
    }

    case OP_EQUAL: {
      Value b = pop();
      Value a = pop();
      push(BOOL_VAL(valuesEqual(a, b)));
      break;
    }

    case OP_FALSE: {
      push(BOOL_VAL(false));
      break;
    }

    case OP_GLOBAL_DEFINE: {
      ObjString* name = READ_STRING();
      tableSet(&vm_.globals, name, peek(0));
      pop();
      break;
    }

    case OP_GLOBAL_GET: {
      ObjString* name = READ_STRING();
      Value value;
      if (!tableGet(&vm_.globals, name, &value)) {
        runtimeError("Undefined variable '%s'.", name->chars);
        return INTERPRET_RUNTIME_ERROR;
      }
      push(value);
      break;
    }

    case OP_GLOBAL_SET: {
      ObjString* name = READ_STRING();
      if (tableSet(&vm_.globals, name, peek(0))) {
        tableDelete(&vm_.globals, name);
        runtimeError("Undefined variable '%s'.", name->chars);
        return INTERPRET_RUNTIME_ERROR;
      }
      break;
    }

    case OP_GREATER: {
      BINARY_OP(BOOL_VAL, >);
      break;
    }

    case OP_INHERIT: {
      Value superclass = peek(1);
      if (!IS_CLASS(superclass)) {
        runtimeError("Superclass must be a class.");
        return INTERPRET_RUNTIME_ERROR;
      }
      ObjClass* subclass = AS_CLASS(peek(0));
      tableAddAll(&AS_CLASS(superclass)->methods, &subclass->methods);
      pop();
      break;
    }

    case OP_INVOKE: {
      ObjString* method = READ_STRING();
      int argCount = readByte(framePtr);
      if (!invoke(method, argCount)) {
        return INTERPRET_RUNTIME_ERROR;
      }
      (*framePtr) = &vm_.current->frames[vm_.current->frameCount - 1];
      break;
    }

    case OP_INVOKE_SUPER: {
      ObjString* method = READ_STRING();
      int argCount = readByte(framePtr);
      ObjClass* superclass = AS_CLASS(pop());
      if (!invokeFromClass(superclass, method, argCount)) {
        return INTERPRET_RUNTIME_ERROR;
      }
      (*framePtr) = &vm_.current->frames[vm_.current->frameCount - 1];
      break;
    }

    case OP_JUMP: {
      uint16_t offset = READ_SHORT();
      (*framePtr)->ip += offset;
      break;
    }

    case OP_JUMP_IF_FALSE: {
      uint16_t offset = READ_SHORT();
      if (isFalsey(peek(0))) {
        (*framePtr)->ip += offset;
      }
      break;
    }

    case OP_LESS: {
      BINARY_OP(BOOL_VAL, <);
      break;
    }

    case OP_LOCAL_GET: {
      Byte slot = readByte(framePtr);
      push((*framePtr)->slots[slot]);
      break;
    }

    case OP_LOCAL_SET: {
      Byte slot = readByte(framePtr);
      (*framePtr)->slots[slot] = peek(0);
      break;
    }

    case OP_LOOP: {
      uint16_t offset = READ_SHORT();
      (*framePtr)->ip -= offset;
      break;
    }

    case OP_METHOD: {
      defineMethod(READ_STRING());
      break;
    }

    case OP_MULTIPLY: {
      BINARY_OP(NUMBER_VAL, *);
      break;
    }

    case OP_NEGATE: {
      if (!IS_NUMBER(peek(0))) {
        runtimeError("Operand must be a number.");
        return INTERPRET_RUNTIME_ERROR;
      }
      push(NUMBER_VAL(-AS_NUMBER(pop())));
      break;
    }

    case OP_NIL: {
      push(NIL_VAL);
      break;
    }

    case OP_NOT: {
      push(BOOL_VAL(isFalsey(pop())));
      break;
    }

    case OP_POP: {
      pop();
      break;
    }

    case OP_PROPERTY_GET: {
      if (!IS_INSTANCE(peek(0))) {
        runtimeError("Only instances have properties.");
        return INTERPRET_RUNTIME_ERROR;
      }

      ObjInstance* instance = AS_INSTANCE(peek(0));
      ObjString* name = READ_STRING();

      Value value;
      if (tableGet(&instance->fields, name, &value)) {
        pop(); // Instance.
        push(value);
        break;
      }

      if (!bindMethod(instance->klass, name)) {
        return INTERPRET_RUNTIME_ERROR;
      }
      break;
    }

    case OP_PROPERTY_SET: {
      if (!IS_INSTANCE(peek(1))) {
        runtimeError("Only instances have fields.");
        return INTERPRET_RUNTIME_ERROR;
      }
      ObjInstance* instance = AS_INSTANCE(peek(1));
      tableSet(&instance->fields, READ_STRING(), peek(0));
      Value value = pop();
      pop();
      push(value);
      break;
    }

    case OP_RETURN: {
      Value result = pop();
      closeUpvalues((*framePtr)->slots);
      vm_.current->frameCount--;
      if (vm_.current->frameCount == 0) {
        pop();
        return INTERPRET_OK;
      }
      vm_.current->stackTop = (*framePtr)->slots;
      push(result);
      (*framePtr) = &vm_.current->frames[vm_.current->frameCount - 1];
      break;
    }

    case OP_SUBTRACT: {
      BINARY_OP(NUMBER_VAL, -);
      break;
    }

    case UP_SUPER_GET: {
      ObjString* name = READ_STRING();
      ObjClass* superclass = AS_CLASS(pop());
      if (!bindMethod(superclass, name)) {
        return INTERPRET_RUNTIME_ERROR;
      }
      break;
    }

    case OP_TRUE: {
      push(BOOL_VAL(true));
      break;
    }

    case OP_UPVALUE_GET: {
      Byte slot = readByte(framePtr);
      push(*(*framePtr)->closure->upvalues[slot]->location);
      break;
    }

    case OP_UPVALUE_SET: {
      Byte slot = readByte(framePtr);
      *(*framePtr)->closure->upvalues[slot]->location = peek(0);
      break;
    }

    case OP_UPVALUE_CLOSE: {
      closeUpvalues(vm_.current->stackTop - 1);
      pop();
      break;
    }

    default : {
      runtimeError("Unknown instruction.");
      return INTERPRET_RUNTIME_ERROR;
    }
  }

  return INTERPRET_CONTINUE;

#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_OP
}

static InterpretResult run() {
  CallFrame* frame = &vm_.current->frames[vm_.current->frameCount - 1];
  InterpretResult result = INTERPRET_CONTINUE;
  while (result == INTERPRET_CONTINUE) {
    if (config_.dbg_exec) {
      traceExecution(vm_.current, frame);
    }
    Byte instruction = readByte(&frame);
    result = runSingle(&vm_.current, &frame, instruction);
  }
  return result;
}

InterpretResult interpret(const char* source) {
  ObjFunction* function = compile(source);
  if (function == NULL) {
    return INTERPRET_COMPILE_ERROR;
  }

  push(OBJ_VAL(function));
  ObjClosure* closure = newClosure(function);
  pop();
  push(OBJ_VAL(closure));
  call(closure, 0);

  return run();
}
