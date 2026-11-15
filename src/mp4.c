/* minimp4 is upstream-clean but not -Wall-clean. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"
