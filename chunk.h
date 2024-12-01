#ifndef chunk_h
#define chunk_h

#include "common.h"
#include "value.h"

typedef enum {
  OP_ADD,
  OP_CALL,
  OP_CALL_POSTFIX,
  OP_CLASS,
  OP_CLOSURE,
  OP_COLLECTION_LIST,
  OP_COLLECTION_TABLE,
  OP_CONSTANT,
  OP_DIVIDE,
  OP_EQUAL,
  OP_FALSE,
  OP_GLOBAL_DEFINE,
  OP_GLOBAL_GET,
  OP_GLOBAL_SET,
  OP_GREATER,
  OP_INHERIT,
  OP_INVOKE,
  OP_INVOKE_SUPER,
  OP_JUMP,
  OP_JUMP_IF_FALSE,
  OP_LESS,
  OP_LOCAL_GET,
  OP_LOCAL_SET,
  OP_LOOP,
  OP_METHOD,
  OP_MULTIPLY,
  OP_NEGATE,
  OP_NIL,
  OP_NOT,
  OP_POP,
  OP_PROPERTY_GET,
  OP_PROPERTY_SET,
  OP_RETURN,
  OP_SUBTRACT,
  OP_TRUE,
  OP_UPVALUE_CLOSE,
  OP_UPVALUE_GET,
  OP_UPVALUE_SET,
  UP_SUPER_GET
} OpCode;

typedef struct {
  int count;
  int capacity;
  Byte* code;
  int* lines;
  ValueArray constants;
} Chunk;

void initChunk(Chunk* chunk);
void freeChunk(Chunk* chunk);
void writeChunk(Chunk* chunk, Byte byte, int line);
int addConstant(Chunk* chunk, Value value);

#endif
