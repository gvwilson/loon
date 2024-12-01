#include <stdio.h>

#include "common.h"
#include "debug.h"
#include "object.h"
#include "value.h"
#include "vm.h"

void disassembleChunk(Chunk* chunk, const char* name) {
  print("== %s ==\n", name);
  for (int offset = 0; offset < chunk->count;) {
    offset = disassembleInstruction(chunk, offset);
  }
}

static int constantInstruction(const char* name, Chunk* chunk, int offset) {
  Byte constant = chunk->code[offset + 1];
  print("%-16s %4d '", name, constant);
  printValue(chunk->constants.values[constant]);
  print("'\n");
  return offset + 2;
}

static int invokeInstruction(const char* name, Chunk* chunk, int offset) {
  Byte constant = chunk->code[offset + 1];
  Byte argCount = chunk->code[offset + 2];
  print("%-16s (%d args) %4d '", name, argCount, constant);
  printValue(chunk->constants.values[constant]);
  print("'\n");
  return offset + 3;
}

static int simpleInstruction(const char* name, int offset) {
  print("%s\n", name);
  return offset + 1;
}

static int byteInstruction(const char* name, Chunk* chunk, int offset) {
  Byte slot = chunk->code[offset + 1];
  print("%-16s %4d\n", name, slot);
  return offset + 2;
}

static int jumpInstruction(const char* name, int sign, Chunk* chunk, int offset) {
  uint16_t jump = (uint16_t)(chunk->code[offset + 1] << BYTE_WIDTH);
  jump |= chunk->code[offset + 2];
  print("%-16s %4d -> %d\n", name, offset, offset + 3 + sign * jump);
  return offset + 3;
}

static int closureInstruction(const char* name, Chunk* chunk, int offset) {
  offset++;
  Byte constant = chunk->code[offset++];
  print("%-16s %4d ", name, constant);
  printValue(chunk->constants.values[constant]);
  print("\n");

  ObjFunction* function = AS_FUNCTION(chunk->constants.values[constant]);
  for (int j = 0; j < function->upvalueCount; j++) {
    int isLocal = chunk->code[offset++];
    int index = chunk->code[offset++];
    print("%04d      |                     %s %d\n",
	  offset - 2, isLocal ? "local" : "upvalue", index);
  }

  return offset;
}

int disassembleInstruction(Chunk* chunk, int offset) {
  print("%04d ", offset);
  if ((offset > 0) && (chunk->lines[offset] == chunk->lines[offset - 1])) {
    print("   | ");
  }
  else {
    print("%4d ", chunk->lines[offset]);
  }

  Byte instruction = chunk->code[offset];
  switch (instruction) {
    case OP_ADD: return simpleInstruction("OP_ADD", offset);
    case OP_CALL: return byteInstruction("OP_CALL", chunk, offset);
    case OP_CALL_POSTFIX: return byteInstruction("OP_CALL_POSTFIX", chunk, offset);
    case OP_CLASS: return constantInstruction("OP_CLASS", chunk, offset);
    case OP_CLOSURE: return closureInstruction("OP_CLOSURE", chunk, offset);
    case OP_COLLECTION_LIST: return simpleInstruction("OP_COLLECTION_LIST", offset);
    case OP_COLLECTION_TABLE: return simpleInstruction("OP_COLLECTION_TABLE", offset);
    case OP_CONSTANT: return constantInstruction("OP_CONSTANT", chunk, offset);
    case OP_DIVIDE: return simpleInstruction("OP_DIVIDE", offset);
    case OP_EQUAL: return simpleInstruction("OP_EQUAL", offset);
    case OP_FALSE: return simpleInstruction("OP_FALSE", offset);
    case OP_GLOBAL_DEFINE: return constantInstruction("OP_GLOBAL_DEFINE", chunk, offset);
    case OP_GLOBAL_GET: return constantInstruction("OP_GLOBAL_GET", chunk, offset);
    case OP_GLOBAL_SET: return constantInstruction("OP_GLOBAL_SET", chunk, offset);
    case OP_GREATER: return simpleInstruction("OP_GREATER", offset);
    case OP_INHERIT: return simpleInstruction("OP_INHERIT", offset);
    case OP_INVOKE: return invokeInstruction("OP_INVOKE", chunk, offset);
    case OP_INVOKE_SUPER: return invokeInstruction("OP_INVOKE_SUPER", chunk, offset);
    case OP_JUMP: return jumpInstruction("OP_JUMP", 1, chunk, offset);
    case OP_JUMP_IF_FALSE: return jumpInstruction("OP_JUMP_IF_FALSE", 1, chunk, offset);
    case OP_LESS: return simpleInstruction("OP_LESS", offset);
    case OP_LOCAL_GET: return byteInstruction("OP_LOCAL_GET", chunk, offset);
    case OP_LOCAL_SET: return byteInstruction("OP_LOCAL_SET", chunk, offset);
    case OP_LOOP: return jumpInstruction("OP_LOOP", -1, chunk, offset);
    case OP_METHOD: return constantInstruction("OP_METHOD", chunk, offset);
    case OP_MULTIPLY: return simpleInstruction("OP_MULTIPLY", offset);
    case OP_NEGATE: return simpleInstruction("OP_NEGATE", offset);
    case OP_NIL: return simpleInstruction("OP_NIL", offset);
    case OP_NOT: return simpleInstruction("OP_NOT", offset);
    case OP_POP: return simpleInstruction("OP_POP", offset);
    case OP_PROPERTY_GET: return constantInstruction("OP_PROPERTY_GET", chunk, offset);
    case OP_PROPERTY_SET: return constantInstruction("OP_PROPERTY_SET", chunk, offset);
    case OP_RETURN: return simpleInstruction("OP_RETURN", offset);
    case OP_SUBTRACT: return simpleInstruction("OP_SUBTRACT", offset);
    case OP_TRUE: return simpleInstruction("OP_TRUE", offset);
    case OP_UPVALUE_CLOSE: return simpleInstruction("OP_UPVALUE_CLOSE", offset);
    case OP_UPVALUE_GET: return byteInstruction("OP_UPVALUE_GET", chunk, offset);
    case OP_UPVALUE_SET: return byteInstruction("OP_UPVALUE_SET", chunk, offset);
    case UP_SUPER_GET: return constantInstruction("UP_SUPER_GET", chunk, offset);
    default:
      print("Unknown opcode %d\n", instruction);
      return offset + 1;
  }
}

void traceExecution(ObjFiber* fiber, CallFrame* frame) {
  print("  %4d> ", fiber->id);
  for (Value* slot = vm_.current->stack; slot < vm_.current->stackTop; slot++) {
    print("[ ");
    printValue(*slot);
    print(" ]");
  }
  print("\n");
  disassembleInstruction(&frame->closure->function->chunk,
                         (int)(frame->ip - frame->closure->function->chunk.code));
}

void printAllObjects() {
  for (Obj* obj = vm_.objects; obj != NULL; obj = obj->next) {
    Value value = OBJ_VAL(obj);
    print("%p %d %s ", obj, obj->type, objectTypeName(obj->type));
    printValue(value);
    print("\n");
  }
}
