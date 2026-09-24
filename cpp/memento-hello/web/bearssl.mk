#
#  bearssl.mk --- which parts of BearSSL the browser is built from, and how.
#
#  Shared by the r2 build (../Makefile) and the host tests (tests/Makefile),
#  so that the TLS code tested on the host is the TLS code that runs on r2.
#
#  Left out: everything that needs vector registers (AES-NI, PCLMUL, SSE2,
#  POWER8), because this kernel does not save them across a context switch;
#  the curve25519 variants that the 64-bit build does not use and that pull in
#  <stdio.h>; and the system RNG, which has nothing to read here --- the engine
#  is seeded by the browser instead (see web_r2.cpp) and sysrng_stub.c answers
#  BearSSL's question about a system source with "none".
#
BEARSSL     ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../third_party/bearssl)
BEARSSL_OUT := aes_x86ni% aes_pwr8% ghash_pclmul% ghash_pwr8% chacha20_sse2% \
               ec_c25519_m31% ec_c25519_m15% ec_c25519_i15% ec_c25519_i31% sysrng%
#  Matched on the file's base name: filter-out takes one '%' per pattern, so
#  "%/aes_x86ni%.c" would not do what it looks like.
BEARSSL_ALL  := $(wildcard $(BEARSSL)/src/*/*.c) $(wildcard $(BEARSSL)/src/*.c)
BEARSSL_SRCS := $(foreach f,$(BEARSSL_ALL),$(if $(filter $(BEARSSL_OUT),$(notdir $(basename $(f)))),,$(f)))

BEARSSL_DEFS := -DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_POWER8=0 -DBR_RDRAND=0 \
                -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 -DBR_USE_UNIX_TIME=0 \
                -DBR_USE_WIN32_RAND=0 -DBR_USE_WIN32_TIME=0 \
                -DBR_64=1 -DBR_INT128=1 -DBR_LE_UNALIGNED=1 -DBR_SLOW_MUL=0

BEARSSL_INC := -I$(BEARSSL)/inc -I$(BEARSSL)/src
