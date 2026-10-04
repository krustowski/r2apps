#include "inner.h"
/* Bounded integer exponentiation for Spotify's 768-bit DH and RSA AP key.
 * BearSSL's i31 path uses integer arithmetic and fixed-sized stack buffers. */
int r2_tls_modexp(unsigned char *out,const unsigned char *base,size_t blen,
 const unsigned char *exponent,size_t elen,const unsigned char *modulus,size_t mlen) {
 uint32_t m[68],x[68],t1[68],t2[68];
 if(!mlen||mlen>256||!elen||elen>256||blen>mlen||!(modulus[mlen-1]&1))return 0;
 br_i31_decode(m,modulus,mlen);
 if(m[0]<2||!br_i31_decode_mod(x,base,blen,m))return 0;
 br_i31_modpow(x,exponent,elen,m,br_i31_ninv31(m[1]),t1,t2);
 br_i31_encode(out,mlen,x);return 1;
}
