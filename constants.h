#ifndef constants_h
#define constants_h

#include "value.h"

#define CONSTANT_STRING(name, value) extern Value name
#include "constants.inc"
#undef CONSTANT_STRING

void initConstants();
void markConstants();

#endif
