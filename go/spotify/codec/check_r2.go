//go:build r2 && r2netcheck

package codec

/*
#include "decoder.h"
*/
import "C"

// NativeCodebookCheck uses the real decoder and kernel allocator on a Go task.
func NativeCodebookCheck() bool { return C.r2v_check_codebook() == 0 }
