package stream

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/sha1"
	"crypto/subtle"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"io"
)

// RFC 2409 group 1, as used by Spotify's AP protocol.
const dhPrimeHex = "ffffffffffffffffc90fdaa22168c234c4c6628b80dc1cd129024e088a67cc74020bbea63b139b22514a08798e3404ddef9519b3cd3a431b302b0a6df25f14374fe1356d6d51c245e485b576625e7ec6f44c42e9a63a3620ffffffffffffffff"

// Spotify AP signature public key (public protocol trust anchor).
const serverKeyHex = "ace0460bffc230aff46bfec3bfbf863da191c6cc336c93a14fb3b01612acac6af180e7f614d9429dbe2e346643e362d2327a1a0d923baedd1402b18155056104d52c96a44c1ecc024ad4b20c001f17edc22fc43521c8f0cbaed2add72b0f9db3c5321a2afe59f35a0dac68f1fa621efb2c8d0cb7392d9247e3d7351a6dbd24c2ae255b88ffab73298a0bcccd0c58673189e8bd3480784a5fc96b899d956bfc86d74f33a6781796c9c32d0d32a5abcd0527e2f710a39613c42f99c027bfed049c3c275804b6b219f9c12f02e94863eca1b642a09d4825f8b39dd0e86af9484da1c2ba863042ea9db3086c190e48b39d66eb0006a25aeea11b13873cd719e655bd"

func verifyAPSignature(data, sig []byte) bool {
	n, _ := hex.DecodeString(serverKeyHex)
	if len(sig) != 256 {
		return false
	}
	em, err := modexp(sig, []byte{1, 0, 1}, n)
	if err != nil {
		return false
	}
	hash := sha1.Sum(data)
	prefix, _ := hex.DecodeString("3021300906052b0e03021a05000414")
	expected := make([]byte, 256)
	expected[1] = 1
	end := 256 - len(prefix) - len(hash)
	for i := 2; i < end-1; i++ {
		expected[i] = 255
	}
	copy(expected[end:], prefix)
	copy(expected[end+len(prefix):], hash[:])
	return subtle.ConstantTimeCompare(em, expected) == 1
}
func GID(id string) ([]byte, error) {
	if len(id) != 22 {
		return nil, errors.New("invalid Spotify track ID")
	}
	out := make([]byte, 16)
	const alphabet = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
	for _, c := range id {
		value := -1
		for i, digit := range alphabet {
			if c == digit {
				value = i
				break
			}
		}
		if value < 0 {
			return nil, errors.New("invalid Spotify track ID")
		}
		carry := value
		for i := 15; i >= 0; i-- {
			carry += int(out[i]) * 62
			out[i] = byte(carry)
			carry >>= 8
		}
		if carry != 0 {
			return nil, errors.New("invalid Spotify track ID")
		}
	}
	return out, nil
}

var audioIV = [16]byte{0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb, 0xcf, 0x77, 0xeb, 0xe8, 0xbc, 0x64, 0x3f, 0x63, 0x0d, 0x93}

type Decryptor struct {
	Reader io.ReaderAt
	block  cipher.Block
}

func NewDecryptor(r io.ReaderAt, key []byte) (*Decryptor, error) {
	if len(key) != 16 {
		return nil, errors.New("Spotify audio key must be 16 bytes")
	}
	b, e := aes.NewCipher(key)
	return &Decryptor{Reader: r, block: b}, e
}
func (d *Decryptor) ReadAt(p []byte, offset int64) (int, error) {
	if offset < 0 {
		return 0, errors.New("negative audio offset")
	}
	iv := audioIV
	binary.BigEndian.PutUint64(iv[8:], binary.BigEndian.Uint64(iv[8:])+uint64(offset/16))
	ctr := cipher.NewCTR(d.block, iv[:])
	var discard [16]byte
	ctr.XORKeyStream(discard[:offset%16], discard[:offset%16])
	n, e := d.Reader.ReadAt(p, offset)
	ctr.XORKeyStream(p[:n], p[:n])
	return n, e
}
