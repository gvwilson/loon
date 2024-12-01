#include "constants.h"
#include "memory.h"
#include "object.h"

#define CONSTANT_STRING(name, value) Value name
#include "constants.inc"
#undef CONSTANT_STRING

static Value makeString(const char* s) {
  return OBJ_VAL(copyString(s, strlen(s)));
}

void initConstants() {
#define CONSTANT_STRING(name, value) name = makeString(value)
#include "constants.inc"
#undef CONSTANT_STRING
}

void markConstants() {
#define CONSTANT_STRING(name, value) markObject(AS_OBJ(name))
#include "constants.inc"
#undef CONSTANT_STRING
}
