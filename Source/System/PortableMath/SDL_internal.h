// Stands in for SDL's own header of this name when the math routines that SDL comes with are built for PortableMath.c, since they need next to nothing from it.
#pragma once

#include <assert.h>
#include <stdint.h>
#include <string.h>

// shortcut: little-endian machines only, which is all that the game runs on.
#define SDL_LIL_ENDIAN 1234
#define SDL_BIG_ENDIAN 4321
#define SDL_FLOATWORDORDER SDL_LIL_ENDIAN

#define SDL_assert(condition) assert(condition)
#define SDL_memset memset
#define SDL_zero(x) memset(&(x), 0, sizeof(x))
#define SDL_arraysize(array) (sizeof(array) / sizeof(array[0]))

typedef uint64_t Uint64;
#define SDL_UINT64_C(value) UINT64_C(value)
