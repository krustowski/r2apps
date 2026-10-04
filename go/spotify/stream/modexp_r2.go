//go:build r2 || r2cryptotest

package stream

/*
#cgo LDFLAGS: -L../../r2tls/build -lbearssl
typedef unsigned long size_t;
int r2_tls_modexp(unsigned char *,const unsigned char *,size_t,const unsigned char *,size_t,const unsigned char *,size_t);
*/
import "C"
import (
	"errors"
	"unsafe"
)

func modexp(base, exponent, modulus []byte) ([]byte, error) {
	if len(base) == 0 || len(exponent) == 0 || len(modulus) == 0 {
		return nil, errors.New("empty modular exponentiation")
	}
	out := make([]byte, len(modulus))
	if C.r2_tls_modexp((*C.uchar)(unsafe.Pointer(&out[0])), (*C.uchar)(unsafe.Pointer(&base[0])), C.size_t(len(base)), (*C.uchar)(unsafe.Pointer(&exponent[0])), C.size_t(len(exponent)), (*C.uchar)(unsafe.Pointer(&modulus[0])), C.size_t(len(modulus))) == 0 {
		return nil, errors.New("invalid modular exponentiation")
	}
	return out, nil
}
