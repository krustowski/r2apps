/*
 *  tcc links crti.o first and crtn.o last into every program, as glibc
 *  needs them to.  r2 has no _init/_fini to frame, so both are this empty
 *  object.
 */
    .text
