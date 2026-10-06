/*
 * tiny_jpeg.h - rou2exOS / libcr2 adaptation
 *
 * Based on Tiny JPEG Encoder by Sergio Gonzalez.
 *
 * Original TinyJPEG is public domain.
 *
 * This adaptation:
 *
 *   - uses libcr2/types.h
 *   - uses libcr2/mem.h
 *   - does not require stdio.h
 *   - does not require assert.h
 *   - does not require string.h
 *   - does not require math.h
 *   - does not require stdlib.h
 *   - removes the FILE-based API
 *   - exposes only tje_encode_with_func()
 *
 * Define TJE_IMPLEMENTATION in exactly one C source file.
 */

#ifndef TJE_HEADER_GUARD
#define TJE_HEADER_GUARD

#include "mem.h"
#include "types.h"

/*
 * libcr2 does not currently expose size_t.
 *
 * TinyJPEG only needs it for small internal buffers, so uint32_t
 * is more than sufficient.
 */
typedef uint32_t tje_size_t;

/* ------------------------------------------------------------- */
/* Public API                                                     */
/* ------------------------------------------------------------- */

typedef void tje_write_func(void *context, void *data, int size);

/*
 * Encode RGB/RGBA image to JPEG.
 *
 * quality:
 *     1 = smallest
 *     2 = good
 *     3 = highest
 *
 * num_components:
 *     3 = RGB
 *     4 = RGBA
 *
 * The output is delivered incrementally through func().
 *
 * Returns:
 *     1 = success
 *     0 = error
 */
int tje_encode_with_func(tje_write_func *func, void *context, int quality, int width, int height, int num_components, const unsigned char *src_data);

#endif /* TJE_HEADER_GUARD */

/* ============================================================= */
/* Implementation                                                 */
/* ============================================================= */

#ifdef TJE_IMPLEMENTATION

/*
 * TinyJPEG's original implementation uses assert().
 *
 * rou2exOS is freestanding and doesn't provide assert.h.
 *
 * Assertions in TinyJPEG are internal sanity checks; disabling them
 * is preferable to pulling a hosted libc dependency into libcr2.
 */
#ifndef assert
#define assert(x) ((void)0)
#endif

/*
 * TinyJPEG uses floorf() for JPEG coefficient rounding.
 *
 * The fast DCT path does not require ceilf(), cosf(), etc.
 *
 * We implement the required floorf locally.
 */
static float tje_floorf(float x) {
    int32_t i;

    i = (int32_t)x;

    if ((float)i > x)
        return (float)(i - 1);

    return (float)i;
}

/*
 * Don't include <math.h>.
 */
#define floorf(x) tje_floorf(x)

/*
 * TinyJPEG normally buffers 1023 bytes before invoking the output
 * callback. This is well within libcr2's uint16_t memcpy() limit.
 */
#define TJEI_BUFFER_SIZE 1024

#define tjei_min(a, b) (((a) < (b)) ? (a) : (b))
#define tjei_max(a, b) (((a) < (b)) ? (b) : (a))

#define TJEI_FORCE_INLINE static

/*
 * The fast AA&N DCT is substantially faster than the generic
 * floating-point DCT and does not require cosf().
 */
#define TJE_USE_FAST_DCT 1

#ifndef TJE_SUBSAMPLING
#define TJE_SUBSAMPLING 444
#endif
#if TJE_SUBSAMPLING != 444 && TJE_SUBSAMPLING != 420
#error "TJE_SUBSAMPLING must be 444 or 420"
#endif
#ifndef TJE_YIELD
#define TJE_YIELD() ((void)0)
#endif

/*
 * libcr2 applications don't have hosted logging.
 */
#define tje_log(msg) ((void)0)

/* ------------------------------------------------------------- */
/* Encoder state                                                   */
/* ------------------------------------------------------------- */

typedef struct {
    void *context;
    tje_write_func *func;
} TJEWriteContext;

typedef struct {
    uint8_t ehuffsize[4][257];
    uint16_t ehuffcode[4][256];

    const uint8_t *ht_bits[4];
    const uint8_t *ht_vals[4];

    uint8_t qt_luma[64];
    uint8_t qt_chroma[64];

    TJEWriteContext write_context;

    uint32_t output_buffer_count;
    uint8_t output_buffer[TJEI_BUFFER_SIZE];

} TJEState;

/* ============================================================= */
/* JPEG tables                                                     */
/* ============================================================= */

static const uint8_t tjei_default_qt_luma_from_spec[] = {16, 11, 10, 16, 24, 40,  51,  61, 12, 12, 14, 19, 26, 58,  60,  55, 14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                                         18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};

static const uint8_t tjei_default_qt_chroma_from_paper[] = {16, 12, 14, 14, 18, 24, 49, 72, 11, 10,  16,  24, 40, 51, 61,  12,  13,  17,  22, 35, 64, 92,  14,  16,  22,  37, 55, 78, 95, 19, 24,  29,
                                                            56, 64, 87, 98, 26, 40, 51, 68, 81, 103, 112, 58, 57, 87, 109, 104, 121, 100, 60, 69, 80, 103, 113, 120, 103, 55, 56, 62, 77, 92, 101, 99};

/* Huffman tables */

static const uint8_t tjei_default_ht_luma_dc_len[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};

static const uint8_t tjei_default_ht_luma_dc[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

static const uint8_t tjei_default_ht_chroma_dc_len[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};

static const uint8_t tjei_default_ht_chroma_dc[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

static const uint8_t tjei_default_ht_luma_ac_len[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};

static const uint8_t tjei_default_ht_luma_ac[] = {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08,
                                                  0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
                                                  0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
                                                  0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
                                                  0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
                                                  0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
                                                  0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA};

static const uint8_t tjei_default_ht_chroma_ac_len[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};

static const uint8_t tjei_default_ht_chroma_ac[] = {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
                                                    0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
                                                    0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
                                                    0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
                                                    0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4,
                                                    0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
                                                    0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA};

/* JPEG zig-zag order */

static const uint8_t tjei_zig_zag[64] = {0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42, 3,  8,  12, 17, 25, 30, 41, 43, 9,  11, 18, 24, 31, 40, 44, 53,
                                         10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60, 21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};

/* ------------------------------------------------------------- */
/* JPEG structures                                                 */
/* ------------------------------------------------------------- */

static uint16_t tjei_be_word(uint16_t value) {
    uint16_t lo = value & 0x00ff;
    uint16_t hi = (value & 0xff00) >> 8;

    return (uint16_t)((lo << 8) | hi);
}

static const uint8_t tjeik_jfif_id[] = "JFIF";
static const uint8_t tjeik_com_str[] = "Created by Tiny JPEG Encoder";

#pragma pack(push)
#pragma pack(1)

typedef struct {
    uint16_t SOI;

    uint16_t APP0;
    uint16_t jfif_len;

    uint8_t jfif_id[5];

    uint16_t version;

    uint8_t units;

    uint16_t x_density;
    uint16_t y_density;

    uint8_t x_thumb;
    uint8_t y_thumb;

} TJEJPEGHeader;

typedef struct {
    uint16_t com;
    uint16_t com_len;

    char com_str[sizeof(tjeik_com_str) - 1];

} TJEJPEGComment;

typedef struct {
    uint8_t component_id;
    uint8_t sampling_factors;
    uint8_t qt;

} TJEComponentSpec;

typedef struct {
    uint16_t SOF;
    uint16_t len;

    uint8_t precision;

    uint16_t height;
    uint16_t width;

    uint8_t num_components;

    TJEComponentSpec component_spec[3];

} TJEFrameHeader;

typedef struct {
    uint8_t component_id;
    uint8_t dc_ac;

} TJEFrameComponentSpec;

typedef struct {
    uint16_t SOS;
    uint16_t len;

    uint8_t num_components;

    TJEFrameComponentSpec component_spec[3];

    uint8_t first;
    uint8_t last;
    uint8_t ah_al;

} TJEScanHeader;

#pragma pack(pop)

/* ------------------------------------------------------------- */
/* Output                                                         */
/* ------------------------------------------------------------- */

static void tjei_write(TJEState *state, const void *data, tje_size_t num_bytes, tje_size_t num_elements) {
    tje_size_t to_write = num_bytes * num_elements;

    tje_size_t available = TJEI_BUFFER_SIZE - 1 - state->output_buffer_count;

    tje_size_t capped_count = tjei_min(to_write, available);

    /*
     * IMPORTANT:
     *
     * libcr2 memcpy() takes uint16_t.
     *
     * capped_count is <= 1023, so this is safe.
     */
    memcpy(state->output_buffer + state->output_buffer_count, data, (uint16_t)capped_count);

    state->output_buffer_count += capped_count;

    if (state->output_buffer_count == TJEI_BUFFER_SIZE - 1) {

        state->write_context.func(state->write_context.context, state->output_buffer, (int)state->output_buffer_count);

        state->output_buffer_count = 0;
    }

    if (capped_count < to_write) {

        tjei_write(state, (const uint8_t *)data + capped_count, to_write - capped_count, 1);
    }
}

static void tjei_write_DQT(TJEState *state, const uint8_t *matrix, uint8_t id) {
    uint16_t DQT = tjei_be_word(0xffdb);

    uint16_t len = tjei_be_word(0x0043);

    uint8_t precision_and_id = id;

    tjei_write(state, &DQT, 2, 1);
    tjei_write(state, &len, 2, 1);
    tjei_write(state, &precision_and_id, 1, 1);

    tjei_write(state, matrix, 64, 1);
}

typedef enum {
    TJEI_DC = 0,
    TJEI_AC = 1

} TJEHuffmanTableClass;

static void tjei_write_DHT(TJEState *state, const uint8_t *matrix_len, const uint8_t *matrix_val, TJEHuffmanTableClass ht_class, uint8_t id) {
    int num_values = 0;

    int i;

    for (i = 0; i < 16; ++i)
        num_values += matrix_len[i];

    uint16_t DHT = tjei_be_word(0xffc4);

    uint16_t len = tjei_be_word((uint16_t)(2 + 1 + 16 + num_values));

    uint8_t tc_th = (uint8_t)(((uint8_t)ht_class << 4) | id);

    tjei_write(state, &DHT, 2, 1);
    tjei_write(state, &len, 2, 1);
    tjei_write(state, &tc_th, 1, 1);
    tjei_write(state, matrix_len, 1, 16);

    tjei_write(state, matrix_val, 1, (tje_size_t)num_values);
}

/* ------------------------------------------------------------- */
/* Huffman                                                        */
/* ------------------------------------------------------------- */

static uint8_t *tjei_huff_get_code_lengths(uint8_t huffsize[256], const uint8_t *bits) {
    int k = 0;
    int i;
    int j;

    for (i = 0; i < 16; ++i) {

        for (j = 0; j < bits[i]; ++j)
            huffsize[k++] = (uint8_t)(i + 1);

        huffsize[k] = 0;
    }

    return huffsize;
}

static uint16_t *tjei_huff_get_codes(uint16_t codes[], uint8_t *huffsize, int64_t count) {
    uint16_t code = 0;

    int k = 0;

    uint8_t sz = huffsize[0];

    for (;;) {

        do {

            assert(k < count);

            codes[k++] = code++;

        } while (huffsize[k] == sz);

        if (huffsize[k] == 0)
            return codes;

        do {

            code = (uint16_t)(code << 1);

            ++sz;

        } while (huffsize[k] != sz);
    }
}

static void tjei_huff_get_extended(uint8_t *out_ehuffsize, uint16_t *out_ehuffcode, const uint8_t *huffval, uint8_t *huffsize, uint16_t *huffcode, int64_t count) {
    int k = 0;

    do {

        uint8_t val = huffval[k];

        out_ehuffcode[val] = huffcode[k];

        out_ehuffsize[val] = huffsize[k];

        ++k;

    } while (k < count);
}

/* ------------------------------------------------------------- */
/* JPEG variable-length integers                                  */
/* ------------------------------------------------------------- */

TJEI_FORCE_INLINE void tjei_calculate_variable_length_int(int value, uint16_t out[2]) {
    int abs_val = value;

    if (value < 0) {
        abs_val = -abs_val;
        --value;
    }

    out[1] = 1;

    while (abs_val >>= 1)
        ++out[1];

    out[0] = (uint16_t)(value & ((1 << out[1]) - 1));
}

/* ------------------------------------------------------------- */
/* Bitstream                                                       */
/* ------------------------------------------------------------- */

/* Entropy output is mostly single bytes. Avoid the freestanding memcpy call
 * and generic recursive writer for each of them. */
static inline void tjei_emit_byte(TJEState *state, uint8_t value) {
    state->output_buffer[state->output_buffer_count++] = value;
    if (state->output_buffer_count == TJEI_BUFFER_SIZE - 1) {
        state->write_context.func(state->write_context.context,
                                 state->output_buffer,
                                 (int)state->output_buffer_count);
        state->output_buffer_count = 0;
    }
}

TJEI_FORCE_INLINE void tjei_write_bits(TJEState *state, uint32_t *bitbuffer, uint32_t *location, uint16_t num_bits, uint16_t bits) {
    uint32_t nloc = *location + num_bits;

    *bitbuffer |= (uint32_t)(bits << (32 - nloc));

    *location = nloc;

    while (*location >= 8) {

        uint8_t c = (uint8_t)((*bitbuffer) >> 24);

        tjei_emit_byte(state, c);

        if (c == 0xff) {

            tjei_emit_byte(state, 0);
        }

        *bitbuffer <<= 8;
        *location -= 8;
    }
}

/* ------------------------------------------------------------- */
/* Fast DCT                                                       */
/* ------------------------------------------------------------- */

static void tjei_fdct(float *data) {
    float tmp0, tmp1, tmp2, tmp3;
    float tmp4, tmp5, tmp6, tmp7;

    float tmp10, tmp11, tmp12, tmp13;

    float z1, z2, z3, z4;
    float z5, z11, z13;

    float *dataptr;

    int ctr;

    dataptr = data;

    for (ctr = 7; ctr >= 0; ctr--) {

        tmp0 = dataptr[0] + dataptr[7];
        tmp7 = dataptr[0] - dataptr[7];

        tmp1 = dataptr[1] + dataptr[6];
        tmp6 = dataptr[1] - dataptr[6];

        tmp2 = dataptr[2] + dataptr[5];
        tmp5 = dataptr[2] - dataptr[5];

        tmp3 = dataptr[3] + dataptr[4];
        tmp4 = dataptr[3] - dataptr[4];

        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;

        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        dataptr[0] = tmp10 + tmp11;
        dataptr[4] = tmp10 - tmp11;

        z1 = (tmp12 + tmp13) * 0.707106781f;

        dataptr[2] = tmp13 + z1;

        dataptr[6] = tmp13 - z1;

        tmp10 = tmp4 + tmp5;
        tmp11 = tmp5 + tmp6;
        tmp12 = tmp6 + tmp7;

        z5 = (tmp10 - tmp12) * 0.382683433f;

        z2 = 0.541196100f * tmp10 + z5;

        z4 = 1.306562965f * tmp12 + z5;

        z3 = tmp11 * 0.707106781f;

        z11 = tmp7 + z3;

        z13 = tmp7 - z3;

        dataptr[5] = z13 + z2;

        dataptr[3] = z13 - z2;

        dataptr[1] = z11 + z4;

        dataptr[7] = z11 - z4;

        dataptr += 8;
    }

    dataptr = data;

    for (ctr = 7; ctr >= 0; ctr--) {

        tmp0 = dataptr[8 * 0] + dataptr[8 * 7];

        tmp7 = dataptr[8 * 0] - dataptr[8 * 7];

        tmp1 = dataptr[8 * 1] + dataptr[8 * 6];

        tmp6 = dataptr[8 * 1] - dataptr[8 * 6];

        tmp2 = dataptr[8 * 2] + dataptr[8 * 5];

        tmp5 = dataptr[8 * 2] - dataptr[8 * 5];

        tmp3 = dataptr[8 * 3] + dataptr[8 * 4];

        tmp4 = dataptr[8 * 3] - dataptr[8 * 4];

        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;

        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        dataptr[8 * 0] = tmp10 + tmp11;

        dataptr[8 * 4] = tmp10 - tmp11;

        z1 = (tmp12 + tmp13) * 0.707106781f;

        dataptr[8 * 2] = tmp13 + z1;

        dataptr[8 * 6] = tmp13 - z1;

        tmp10 = tmp4 + tmp5;
        tmp11 = tmp5 + tmp6;
        tmp12 = tmp6 + tmp7;

        z5 = (tmp10 - tmp12) * 0.382683433f;

        z2 = 0.541196100f * tmp10 + z5;

        z4 = 1.306562965f * tmp12 + z5;

        z3 = tmp11 * 0.707106781f;

        z11 = tmp7 + z3;

        z13 = tmp7 - z3;

        dataptr[8 * 5] = z13 + z2;

        dataptr[8 * 3] = z13 - z2;

        dataptr[8 * 1] = z11 + z4;

        dataptr[8 * 7] = z11 - z4;

        dataptr++;
    }
}

/* ------------------------------------------------------------- */
/* MCU encoding                                                    */
/* ------------------------------------------------------------- */

#define ABS(x) ((x) < 0 ? -(x) : (x))

static void tjei_encode_and_write_MCU(TJEState *state, float *mcu, float *qt, uint8_t *huff_dc_len, uint16_t *huff_dc_code, uint8_t *huff_ac_len, uint16_t *huff_ac_code, int *pred,
                                      uint32_t *bitbuffer, uint32_t *location) {
    int du[64];

    float dct_mcu[64];

    uint16_t vli[2];

    int i;
    int constant = 1;

    /* Desktop backgrounds and indexed games have many solid blocks. Their
     * transform contains only DC: skip the copy, DCT and 63 AC quantizations.
     * Multiplication by 64 exactly matches the AA&N DC sum for equal inputs. */
    for (i = 1; i < 64; ++i) {
        if (mcu[i] != mcu[0]) { constant = 0; break; }
    }
    if (constant) {
        float dc = (mcu[0] * 64.0f) * qt[0];
        du[0] = (int)(floorf(dc + 1024.5f) - 1024.0f);
        goto dc_coefficient;
    }

    /*
     * 64 floats = 256 bytes, safely below libcr2 memcpy's
     * uint16_t length limit.
     */
    memcpy(dct_mcu, mcu, (uint16_t)(64 * sizeof(float)));

    tjei_fdct(dct_mcu);

    for (i = 0; i < 64; ++i) {

        float fval = dct_mcu[i] * qt[i];

        /*
         * Original TinyJPEG uses:
         *
         *     floorf(fval + 1024 + 0.5) - 1024
         *
         * This avoids negative-zero/rounding problems.
         */
        fval = floorf(fval + 1024.5f);

        fval -= 1024.0f;

        du[tjei_zig_zag[i]] = (int)fval;
    }

    /*
     * DC coefficient.
     */

dc_coefficient:
    {
        int diff = du[0] - *pred;

        *pred = du[0];

        if (diff != 0) {

            tjei_calculate_variable_length_int(diff, vli);

            tjei_write_bits(state, bitbuffer, location, huff_dc_len[vli[1]], huff_dc_code[vli[1]]);

            tjei_write_bits(state, bitbuffer, location, vli[1], vli[0]);

        } else {

            tjei_write_bits(state, bitbuffer, location, huff_dc_len[0], huff_dc_code[0]);
        }
    }

    if (constant) {
        tjei_write_bits(state, bitbuffer, location, huff_ac_len[0], huff_ac_code[0]);
        return;
    }

    /*
     * AC coefficients.
     */

    {
        int last_non_zero_i = 0;

        for (i = 63; i > 0; --i) {

            if (du[i] != 0) {
                last_non_zero_i = i;
                break;
            }
        }

        for (i = 1; i <= last_non_zero_i; ++i) {

            int zero_count = 0;

            while (du[i] == 0) {

                ++zero_count;
                ++i;

                if (zero_count == 16) {

                    tjei_write_bits(state, bitbuffer, location, huff_ac_len[0xf0], huff_ac_code[0xf0]);

                    zero_count = 0;
                }
            }

            tjei_calculate_variable_length_int(du[i], vli);

            {
                uint16_t sym1 = (uint16_t)(((uint16_t)zero_count << 4) | vli[1]);

                tjei_write_bits(state, bitbuffer, location, huff_ac_len[sym1], huff_ac_code[sym1]);

                tjei_write_bits(state, bitbuffer, location, vli[1], vli[0]);
            }
        }

        if (last_non_zero_i != 63) {

            tjei_write_bits(state, bitbuffer, location, huff_ac_len[0], huff_ac_code[0]);
        }
    }
}

/* ------------------------------------------------------------- */
/* Huffman state                                                   */
/* ------------------------------------------------------------- */

enum { TJEI_LUMA_DC, TJEI_LUMA_AC, TJEI_CHROMA_DC, TJEI_CHROMA_AC };

struct TJEProcessedQT {
    float chroma[64];
    float luma[64];
};

static void tjei_huff_expand(TJEState *state) {
    static uint8_t huffsize[4][257];

    static uint16_t huffcode[4][256];

    int32_t spec_tables_len[4] = {0, 0, 0, 0};

    int i;
    int k;

    state->ht_bits[TJEI_LUMA_DC] = tjei_default_ht_luma_dc_len;

    state->ht_bits[TJEI_LUMA_AC] = tjei_default_ht_luma_ac_len;

    state->ht_bits[TJEI_CHROMA_DC] = tjei_default_ht_chroma_dc_len;

    state->ht_bits[TJEI_CHROMA_AC] = tjei_default_ht_chroma_ac_len;

    state->ht_vals[TJEI_LUMA_DC] = tjei_default_ht_luma_dc;

    state->ht_vals[TJEI_LUMA_AC] = tjei_default_ht_luma_ac;

    state->ht_vals[TJEI_CHROMA_DC] = tjei_default_ht_chroma_dc;

    state->ht_vals[TJEI_CHROMA_AC] = tjei_default_ht_chroma_ac;

    for (i = 0; i < 4; ++i) {

        for (k = 0; k < 16; ++k)
            spec_tables_len[i] += state->ht_bits[i][k];
    }

    for (i = 0; i < 4; ++i) {

        assert(256 >= spec_tables_len[i]);

        tjei_huff_get_code_lengths(huffsize[i], state->ht_bits[i]);

        tjei_huff_get_codes(huffcode[i], huffsize[i], spec_tables_len[i]);
    }

    for (i = 0; i < 4; ++i) {

        int64_t count = spec_tables_len[i];

        tjei_huff_get_extended(state->ehuffsize[i], state->ehuffcode[i], state->ht_vals[i], huffsize[i], huffcode[i], count);
    }
}

/* ------------------------------------------------------------- */
/* Main JPEG encoder                                               */
/* ------------------------------------------------------------- */

static int tjei_encode_main(TJEState *state, const unsigned char *src_data, int width, int height, int src_num_components) {
    struct TJEProcessedQT pqt;

    static const float aan_scales[] = {1.0f, 1.387039845f, 1.306562965f, 1.175875602f, 1.0f, 0.785694958f, 0.541196100f, 0.275899379f};

    int x;
    int y;

    if (src_num_components != 3 && src_num_components != 4)
        return 0;

    if (width <= 0 || height <= 0)
        return 0;

    if (width > 0xffff || height > 0xffff)
        return 0;

    /*
     * Precalculate quantization tables.
     */

    for (y = 0; y < 8; ++y) {

        for (x = 0; x < 8; ++x) {

            int i = y * 8 + x;

            pqt.luma[i] = 1.0f / (8.0f * aan_scales[x] * aan_scales[y] * state->qt_luma[tjei_zig_zag[i]]);

            pqt.chroma[i] = 1.0f / (8.0f * aan_scales[x] * aan_scales[y] * state->qt_chroma[tjei_zig_zag[i]]);
        }
    }

    /*
     * JFIF header.
     */

    {
        TJEJPEGHeader header;

        header.SOI = tjei_be_word(0xffd8);

        header.APP0 = tjei_be_word(0xffe0);

        header.jfif_len = tjei_be_word(sizeof(TJEJPEGHeader) - 4);

        memcpy(header.jfif_id, tjeik_jfif_id, 5);

        header.version = tjei_be_word(0x0102);

        header.units = 1;

        header.x_density = tjei_be_word(0x0060);

        header.y_density = tjei_be_word(0x0060);

        header.x_thumb = 0;
        header.y_thumb = 0;

        tjei_write(state, &header, sizeof(TJEJPEGHeader), 1);
    }

    /*
     * Comment.
     */

    {
        TJEJPEGComment com;

        uint16_t com_len = (uint16_t)(2 + sizeof(tjeik_com_str) - 1);

        com.com = tjei_be_word(0xfffe);

        com.com_len = tjei_be_word(com_len);

        memcpy(com.com_str, tjeik_com_str, (uint16_t)(sizeof(tjeik_com_str) - 1));

        tjei_write(state, &com, sizeof(TJEJPEGComment), 1);
    }

    /*
     * Quantization tables.
     */

    tjei_write_DQT(state, state->qt_luma, 0);

    tjei_write_DQT(state, state->qt_chroma, 1);

    /*
     * Start Of Frame.
     */

    {
        TJEFrameHeader header;

        uint8_t tables[3] = {0, 1, 1};

        header.SOF = tjei_be_word(0xffc0);

        header.len = tjei_be_word(17);

        header.precision = 8;

        header.width = tjei_be_word((uint16_t)width);

        header.height = tjei_be_word((uint16_t)height);

        header.num_components = 3;

        for (int i = 0; i < 3; ++i) {

            header.component_spec[i].component_id = (uint8_t)(i + 1);

            header.component_spec[i].sampling_factors =
                (TJE_SUBSAMPLING == 420 && i == 0) ? 0x22 : 0x11;

            header.component_spec[i].qt = tables[i];
        }

        tjei_write(state, &header, sizeof(TJEFrameHeader), 1);
    }

    /*
     * Huffman tables.
     */

    tjei_write_DHT(state, state->ht_bits[TJEI_LUMA_DC], state->ht_vals[TJEI_LUMA_DC], TJEI_DC, 0);

    tjei_write_DHT(state, state->ht_bits[TJEI_LUMA_AC], state->ht_vals[TJEI_LUMA_AC], TJEI_AC, 0);

    tjei_write_DHT(state, state->ht_bits[TJEI_CHROMA_DC], state->ht_vals[TJEI_CHROMA_DC], TJEI_DC, 1);

    tjei_write_DHT(state, state->ht_bits[TJEI_CHROMA_AC], state->ht_vals[TJEI_CHROMA_AC], TJEI_AC, 1);

    /*
     * Start Of Scan.
     */

    {
        TJEScanHeader header;

        uint8_t tables[3] = {0x00, 0x11, 0x11};

        header.SOS = tjei_be_word(0xffda);

        header.len = tjei_be_word(6 + sizeof(TJEFrameComponentSpec) * 3);

        header.num_components = 3;

        for (int i = 0; i < 3; ++i) {

            header.component_spec[i].component_id = (uint8_t)(i + 1);

            header.component_spec[i].dc_ac = tables[i];
        }

        header.first = 0;
        header.last = 63;
        header.ah_al = 0;

        tjei_write(state, &header, sizeof(TJEScanHeader), 1);
    }

    /*
     * MCU buffers.
     */

    float du_y[64];
    float du_b[64];
    float du_r[64];

    int pred_y = 0;
    int pred_b = 0;
    int pred_r = 0;

    uint32_t bitbuffer = 0;
    uint32_t location = 0;

    /*
     * Encode 8x8 MCUs.
     */

    for (y = 0; y < height; y += (TJE_SUBSAMPLING == 420 ? 16 : 8)) {
        if ((y & 15) == 0)
            TJE_YIELD();

#if TJE_SUBSAMPLING == 420
        /* A 16x16 MCU contains four Y blocks and one averaged block for each
         * chroma channel. Replicate edge pixels before averaging. */
        for (x = 0; x < width; x += 16) {
            for (int i = 0; i < 64; ++i)
                du_b[i] = du_r[i] = 0.0f;
            for (int block = 0; block < 4; ++block) {
                int bx = (block & 1) * 8;
                int by = (block >> 1) * 8;
                for (int iy = 0; iy < 8; ++iy) {
                    int row = tjei_min(y + by + iy, height - 1);
                    for (int ix = 0; ix < 8; ++ix) {
                        int col = tjei_min(x + bx + ix, width - 1);
                        uint32_t p = ((uint32_t)row * width + col) * src_num_components;
                        float r = src_data[p], g = src_data[p + 1], b = src_data[p + 2];
                        int ci = ((by + iy) / 2) * 8 + (bx + ix) / 2;
                        du_y[iy * 8 + ix] = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
                        du_b[ci] += (-0.1687f * r - 0.3313f * g + 0.5f * b) * 0.25f;
                        du_r[ci] += (0.5f * r - 0.4187f * g - 0.0813f * b) * 0.25f;
                    }
                }
                tjei_encode_and_write_MCU(state, du_y, pqt.luma,
                    state->ehuffsize[TJEI_LUMA_DC], state->ehuffcode[TJEI_LUMA_DC],
                    state->ehuffsize[TJEI_LUMA_AC], state->ehuffcode[TJEI_LUMA_AC],
                    &pred_y, &bitbuffer, &location);
            }
            tjei_encode_and_write_MCU(state, du_b, pqt.chroma,
                state->ehuffsize[TJEI_CHROMA_DC], state->ehuffcode[TJEI_CHROMA_DC],
                state->ehuffsize[TJEI_CHROMA_AC], state->ehuffcode[TJEI_CHROMA_AC],
                &pred_b, &bitbuffer, &location);
            tjei_encode_and_write_MCU(state, du_r, pqt.chroma,
                state->ehuffsize[TJEI_CHROMA_DC], state->ehuffcode[TJEI_CHROMA_DC],
                state->ehuffsize[TJEI_CHROMA_AC], state->ehuffcode[TJEI_CHROMA_AC],
                &pred_r, &bitbuffer, &location);
        }
#else
        for (x = 0; x < width; x += 8) {

            int off_y;
            int off_x;

            for (off_y = 0; off_y < 8; ++off_y) {

                for (off_x = 0; off_x < 8; ++off_x) {

                    int row = y + off_y;

                    int col = x + off_x;

                    /*
                     * Edge pixels are replicated.
                     */
                    if (row >= height)
                        row = height - 1;

                    if (col >= width)
                        col = width - 1;

                    int block_index = off_y * 8 + off_x;

                    uint32_t src_index = (((uint32_t)row * (uint32_t)width) + (uint32_t)col) * (uint32_t)src_num_components;

                    uint8_t r = src_data[src_index + 0];

                    uint8_t g = src_data[src_index + 1];

                    uint8_t b = src_data[src_index + 2];

                    du_y[block_index] = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;

                    du_b[block_index] = -0.1687f * r - 0.3313f * g + 0.5f * b;

                    du_r[block_index] = 0.5f * r - 0.4187f * g - 0.0813f * b;
                }
            }

            tjei_encode_and_write_MCU(state, du_y, pqt.luma, state->ehuffsize[TJEI_LUMA_DC], state->ehuffcode[TJEI_LUMA_DC], state->ehuffsize[TJEI_LUMA_AC], state->ehuffcode[TJEI_LUMA_AC], &pred_y,
                                      &bitbuffer, &location);

            tjei_encode_and_write_MCU(state, du_b, pqt.chroma, state->ehuffsize[TJEI_CHROMA_DC], state->ehuffcode[TJEI_CHROMA_DC], state->ehuffsize[TJEI_CHROMA_AC], state->ehuffcode[TJEI_CHROMA_AC],
                                      &pred_b, &bitbuffer, &location);

            tjei_encode_and_write_MCU(state, du_r, pqt.chroma, state->ehuffsize[TJEI_CHROMA_DC], state->ehuffcode[TJEI_CHROMA_DC], state->ehuffsize[TJEI_CHROMA_AC], state->ehuffcode[TJEI_CHROMA_AC],
                                      &pred_r, &bitbuffer, &location);
        }
#endif
    }

    /*
     * Flush partial byte.
     */

    if (location > 0 && location < 8) {

        tjei_write_bits(state, &bitbuffer, &location, (uint16_t)(8 - location), 0);
    }

    /*
     * End Of Image.
     */

    {
        uint16_t EOI = tjei_be_word(0xffd9);

        tjei_write(state, &EOI, 2, 1);
    }

    /*
     * Flush output.
     */

    if (state->output_buffer_count) {

        state->write_context.func(state->write_context.context, state->output_buffer, (int)state->output_buffer_count);

        state->output_buffer_count = 0;
    }

    return 1;
}

/* ------------------------------------------------------------- */
/* Public encoder                                                  */
/* ------------------------------------------------------------- */

int tje_encode_with_func(tje_write_func *func, void *context, int quality, int width, int height, int num_components, const unsigned char *src_data) {
    /*
     * One static state is enough for the streamer because JPEG
     * encoding is synchronous.
     */
    static TJEState state;

    uint8_t qt_factor = 1;

    int i;

    if (!func || !src_data)
        return 0;

    if (quality < 1 || quality > 3)
        return 0;

    /*
     * Reset the state completely.
     *
     * This is important because the same encoder state is reused
     * for every video frame.
     */
    for (i = 0; i < (int)sizeof(TJEState); ++i) {

        ((uint8_t *)&state)[i] = 0;
    }

    /*
     * Quantization tables.
     */

    switch (quality) {

    case 3:

        for (i = 0; i < 64; ++i) {

            state.qt_luma[i] = 1;
            state.qt_chroma[i] = 1;
        }

        break;

    case 2:

        qt_factor = 10;

        /* fall through */

    case 1:

        for (i = 0; i < 64; ++i) {

            state.qt_luma[i] = tjei_default_qt_luma_from_spec[i] / qt_factor;

            if (state.qt_luma[i] == 0)
                state.qt_luma[i] = 1;

            state.qt_chroma[i] = tjei_default_qt_chroma_from_paper[i] / qt_factor;

            if (state.qt_chroma[i] == 0)
                state.qt_chroma[i] = 1;
        }

        break;
    }

    state.write_context.context = context;

    state.write_context.func = func;

    tjei_huff_expand(&state);

    return tjei_encode_main(&state, src_data, width, height, num_components);
}

#endif /* TJE_IMPLEMENTATION */
