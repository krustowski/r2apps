/*  pl_mpeg's <stdlib.h>.  Its allocation goes through PLM_MALLOC (see
 *  main.cpp), so all it still wants from here is abs().  */
#pragma once
static inline int abs(int x) { return x < 0 ? -x : x; }

#ifndef NULL
#define NULL nullptr
#endif
