//go:build !r2 && !r2cryptotest

package stream

import (
	"errors"
	"math/big"
)

func modexp(base, exponent, modulus []byte) ([]byte, error) {
	n := new(big.Int).SetBytes(modulus)
	b := new(big.Int).SetBytes(base)
	if len(modulus) == 0 || len(modulus) > 256 || len(exponent) == 0 || len(exponent) > 256 || len(base) > len(modulus) || n.Cmp(big.NewInt(1)) <= 0 || n.Bit(0) == 0 || b.Cmp(n) >= 0 {
		return nil, errors.New("invalid modular exponentiation")
	}
	return new(big.Int).Exp(b, new(big.Int).SetBytes(exponent), n).FillBytes(make([]byte, len(modulus))), nil
}
