#ifndef common_h
#define common_h

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <stdio.h> // FIXME

#define NAN_BOXING

typedef uint8_t Byte;
#define BYTE_WIDTH 8
#define BYTE_MASK 0xFF
#define BYTE_MAX UINT8_MAX
#define BYTE_HEIGHT (BYTE_MAX + 1)

void print(const char* fmt, ...);

#endif
