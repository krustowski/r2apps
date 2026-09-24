/*
 *  sysrng_stub.c --- BearSSL's "system RNG" on a machine that has none.
 *
 *  BearSSL asks for a system seeder when a PRNG is initialised without enough
 *  injected entropy.  There is nothing to read on r2, so the answer is "no
 *  seeder" and the engine relies on what the browser injects before every
 *  handshake (web_r2.cpp gathers it).  An engine that somehow got no entropy
 *  refuses to start rather than running on a predictable seed.
 */

#include "bearssl.h"

br_prng_seeder br_prng_seeder_system(const char **name)
{
    if (name)
        *name = "none";
    return 0;
}
