#ifndef _R2_PRINTF_INCLUDED_
#define _R2_PRINTF_INCLUDED_ 

/*
 *  printf.h
 *
 *  krusty@vxn.dev / Aug 8, 2025
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "args.h"
#include "syscall.h"

/*
 *  static void print_string() prototype
 *
 *  This function is to print the given uint8_t array reference to the standard output.
 */
static void print_string(const uint8_t *s);

/*
 *  static void print_decimal() prototype
 *
 *  This function is to convert a signed 32bit integer into the string representation.
 *  Each character is printed out directly by the function.
 */
static void print_decimal(int val);

/*
 *  static void print_unsigned() prototype
 *
 *  This function is to convert an unsigned 32bit integer into the string representation.
 *  Each character is printed out directly by the function.
 */
static void print_unsigned(unsigned int val);

/*
 *  static void print_hex() prototype
 *
 *  This function is to take an unsigned 32 bit integer and write it out to the standard
 *  output as a hexadecimal representation. Each character is printed out directly by the function.
 */
static void print_hex(unsigned int val);

/*
 *  void printf() prototype
 *
 *  The main metafunction to parse the formatting string and print values provided in arguments 
 *  accordingly.
 */
void printf(const uint8_t *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif

