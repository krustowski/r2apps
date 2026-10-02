//
//  mp2.hpp --- an MPEG-1 Audio Layer II decoder in integers.
//
//  pl_mpeg's plm_audio (MIT, Dominic Szablewski), which is kjmp2 by Martin J.
//  Fiedler, with its synthesis filterbank taken back from float to integers:
//  the player may not use a floating-point register (README.md), and kjmp2
//  was fixed-point to begin with.  The header, the bit allocation, the scale
//  factors and the dequantisation are pl_mpeg's integer code as it is; the
//  32-point DCT and the 512-tap window are generated as integers from
//  pl_mpeg's float ones by tools/gen_mp2.py (mp2_synth.inc).  Checked against
//  pl_mpeg's float decoder on the host (tests/mp2_compare.cpp).
//
//  Reads from a plm_buffer_t (the demuxer's audio packets) and gives 1152
//  samples a frame as signed 16-bit stereo, left then right; a mono stream is
//  given in both channels.  Include after pl_mpeg_r2.h.
//
#pragma once

#include "mp2_synth.inc"

class Mp2Decoder
{
public:
    static const int SAMPLES = 1152;
    //  The samples go into the filterbank 2^6 times larger than pl_mpeg's
    //  (up to about 2^23; the DCT's sums then reach 2^28), and the factor
    //  comes out again at the end.
    static const int PRE_SHIFT = 6, PRE = 1 << PRE_SHIFT;

    explicit Mp2Decoder(plm_buffer_t *buffer) : buffer_(buffer) {}

    //  The sample rate, once a header has been read; 0 before.
    int rate()
    {
        if (!hasHeader_ && !nextSize_)
            nextSize_ = decodeHeader();
        return hasHeader_ ? kRate[samplerate_] : 0;
    }

    //  One frame into out() (SAMPLES * 2 values); false while the buffer does
    //  not hold a whole one yet.
    bool decode()
    {
        if (!nextSize_)
        {
            if (!plm_buffer_has(buffer_, 48))
                return false;
            nextSize_ = decodeHeader();
        }
        if (!nextSize_ || !plm_buffer_has(buffer_, (size_t)nextSize_ << 3))
            return false;
        decodeFrame();
        nextSize_ = 0;
        return true;
    }

    const int16_t *out() const { return out_; }

    void rewind()
    {
        nextSize_ = 0;
        vpos_ = 0;
        memset(V_, 0, sizeof(V_));
    }

private:
    struct QSpec
    {
        uint16_t levels;
        uint8_t group;
        uint8_t bits;
    };

    static constexpr uint16_t kRate[8] = {44100, 48000, 32000, 0, 22050, 24000, 16000, 0};
    static constexpr int16_t kBitRate[28] = {32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384,
                                             8,  16, 24, 32, 40, 48, 56,  64,  80,  96,  112, 128, 144, 160};
    static constexpr int kScaleBase[3] = {0x02000000, 0x01965FEA, 0x01428A30};
    static constexpr uint8_t kLut1[2][16] = {
        {0, 0, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2},
        {0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 2, 2},
    };
    static constexpr uint8_t kLut2[3][3] = {
        {8, 8, 12},
        {27 | 64, 27 | 64, 27 | 64},
        {30 | 64, 27 | 64, 30 | 64},
    };
    static constexpr uint8_t kLut3[3][32] = {
        {0x44, 0x44, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34},
        {0x43, 0x43, 0x43, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x31, 0x31, 0x31, 0x31, 0x31,
         0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20},
        {0x45, 0x45, 0x45, 0x45, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x24, 0x24, 0x24, 0x24,
         0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24},
    };
    static constexpr uint8_t kLut4[6][16] = {
        {0, 1, 2, 17},
        {0, 1, 2, 3, 4, 5, 6, 17},
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 17},
        {0, 1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17},
        {0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17},
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    };
    static constexpr QSpec kQuant[17] = {
        {3, 1, 5},       {5, 1, 7},       {7, 0, 3},       {9, 1, 10},      {15, 0, 4},    {31, 0, 5},
        {63, 0, 6},      {127, 0, 7},     {255, 0, 8},     {511, 0, 9},     {1023, 0, 10}, {2047, 0, 11},
        {4095, 0, 12},   {8191, 0, 13},   {16383, 0, 14},  {32767, 0, 15},  {65535, 0, 16},
    };

    plm_buffer_t *buffer_;
    int samplerate_ = 3, bitrate_ = 0, mode_ = 0, bound_ = 0, vpos_ = 0, nextSize_ = 0;
    bool hasHeader_ = false;
    const QSpec *allocation_[2][32] = {};
    uint8_t sfInfo_[2][32] = {};
    int sf_[2][32][3] = {};
    int sample_[2][32][3] = {};
    int32_t V_[2][1024] = {};
    int16_t out_[SAMPLES * 2] = {};

    bool findSync()
    {
        size_t i;
        for (i = buffer_->bit_index >> 3; i + 1 < buffer_->length; i++)
            if (buffer_->bytes[i] == 0xFF && (buffer_->bytes[i + 1] & 0xFE) == 0xFC)
            {
                buffer_->bit_index = ((i + 1) << 3) + 3;
                return true;
            }
        buffer_->bit_index = (i + 1) << 3;
        return false;
    }

    int decodeHeader()
    {
        if (!plm_buffer_has(buffer_, 48))
            return 0;
        plm_buffer_skip_bytes(buffer_, 0x00);
        int sync = plm_buffer_read(buffer_, 11);
        if (sync != 0x7FF && !findSync())
            return 0;
        int version = plm_buffer_read(buffer_, 2);
        int layer = plm_buffer_read(buffer_, 2);
        int hasCRC = !plm_buffer_read(buffer_, 1);
        if (version != 3 || layer != 2) // MPEG-1, layer II
            return 0;
        int bitrate = plm_buffer_read(buffer_, 4) - 1;
        if (bitrate < 0 || bitrate > 13)
            return 0;
        int samplerate = plm_buffer_read(buffer_, 2);
        if (samplerate == 3)
            return 0;
        int padding = plm_buffer_read(buffer_, 1);
        plm_buffer_skip(buffer_, 1);
        int mode = plm_buffer_read(buffer_, 2);
        //  A header unlike the last: sync was probably missed.
        if (hasHeader_ && (bitrate_ != bitrate || samplerate_ != samplerate || mode_ != mode))
            return 0;
        bitrate_ = bitrate;
        samplerate_ = samplerate;
        mode_ = mode;
        hasHeader_ = true;
        if (mode == 1) // joint stereo
            bound_ = (plm_buffer_read(buffer_, 2) + 1) << 2;
        else
        {
            plm_buffer_skip(buffer_, 2);
            bound_ = mode == 3 ? 0 : 32;
        }
        plm_buffer_skip(buffer_, 4);
        if (hasCRC)
            plm_buffer_skip(buffer_, 16);
        int size = 144000 * kBitRate[bitrate_] / kRate[samplerate_] + padding;
        return size - (hasCRC ? 6 : 4);
    }

    const QSpec *readAllocation(int sb, int tab3)
    {
        int tab4 = kLut3[tab3][sb];
        int q = kLut4[tab4 & 15][plm_buffer_read(buffer_, tab4 >> 4)];
        return q ? &kQuant[q - 1] : nullptr;
    }

    void readSamples(int ch, int sb, int part)
    {
        const QSpec *q = allocation_[ch][sb];
        int sf = sf_[ch][sb][part];
        int *s = sample_[ch][sb];
        if (!q)
        {
            s[0] = s[1] = s[2] = 0;
            return;
        }
        if (sf == 63)
            sf = 0;
        else
        {
            int shift = sf / 3;
            sf = (kScaleBase[sf % 3] + ((1 << shift) >> 1)) >> shift;
        }
        int adj = q->levels;
        if (q->group)
        {
            int val = plm_buffer_read(buffer_, q->bits);
            s[0] = val % adj;
            val /= adj;
            s[1] = val % adj;
            s[2] = val / adj;
        }
        else
        {
            s[0] = plm_buffer_read(buffer_, q->bits);
            s[1] = plm_buffer_read(buffer_, q->bits);
            s[2] = plm_buffer_read(buffer_, q->bits);
        }
        int scale = 65536 / (adj + 1);
        adj = ((adj + 1) >> 1) - 1;
        for (int k = 0; k < 3; k++)
        {
            int val = (adj - s[k]) * scale;
            //  Six bits more than pl_mpeg keeps: the DCT truncates at every
            //  step, and its inputs (about +-2^17 at full scale) are small
            //  enough to leave the room (see decodeFrame).
            s[k] = ((val * (sf >> 12) + ((val * (sf & 4095) + 2048) >> 12)) >> 12) * PRE;
        }
    }

    void decodeFrame()
    {
        int tab1 = mode_ == 3 ? 0 : 1;
        int tab2 = kLut1[tab1][bitrate_];
        int tab3 = kLut2[tab2][samplerate_];
        int sblimit = tab3 & 63;
        tab3 >>= 6;
        if (bound_ > sblimit)
            bound_ = sblimit;

        for (int sb = 0; sb < bound_; sb++)
        {
            allocation_[0][sb] = readAllocation(sb, tab3);
            allocation_[1][sb] = readAllocation(sb, tab3);
        }
        for (int sb = bound_; sb < sblimit; sb++)
            allocation_[0][sb] = allocation_[1][sb] = readAllocation(sb, tab3);

        int channels = mode_ == 3 ? 1 : 2;
        for (int sb = 0; sb < sblimit; sb++)
        {
            for (int ch = 0; ch < channels; ch++)
                if (allocation_[ch][sb])
                    sfInfo_[ch][sb] = (uint8_t)plm_buffer_read(buffer_, 2);
            if (mode_ == 3)
                sfInfo_[1][sb] = sfInfo_[0][sb];
        }
        for (int sb = 0; sb < sblimit; sb++)
        {
            for (int ch = 0; ch < channels; ch++)
                if (allocation_[ch][sb])
                {
                    int *sf = sf_[ch][sb];
                    switch (sfInfo_[ch][sb])
                    {
                    case 0:
                        sf[0] = plm_buffer_read(buffer_, 6);
                        sf[1] = plm_buffer_read(buffer_, 6);
                        sf[2] = plm_buffer_read(buffer_, 6);
                        break;
                    case 1:
                        sf[0] = sf[1] = plm_buffer_read(buffer_, 6);
                        sf[2] = plm_buffer_read(buffer_, 6);
                        break;
                    case 2:
                        sf[0] = sf[1] = sf[2] = plm_buffer_read(buffer_, 6);
                        break;
                    case 3:
                        sf[0] = plm_buffer_read(buffer_, 6);
                        sf[1] = sf[2] = plm_buffer_read(buffer_, 6);
                        break;
                    }
                }
            if (mode_ == 3)
                for (int k = 0; k < 3; k++)
                    sf_[1][sb][k] = sf_[0][sb][k];
        }

        int outPos = 0;
        for (int part = 0; part < 3; part++)
            for (int granule = 0; granule < 4; granule++)
            {
                for (int sb = 0; sb < bound_; sb++)
                {
                    readSamples(0, sb, part);
                    readSamples(1, sb, part);
                }
                for (int sb = bound_; sb < sblimit; sb++)
                {
                    readSamples(0, sb, part);
                    for (int k = 0; k < 3; k++)
                        sample_[1][sb][k] = sample_[0][sb][k];
                }
                for (int sb = sblimit; sb < 32; sb++)
                    for (int k = 0; k < 3; k++)
                        sample_[0][sb][k] = sample_[1][sb][k] = 0;

                for (int p = 0; p < 3; p++)
                {
                    vpos_ = (vpos_ - 64) & 1023;
                    for (int ch = 0; ch < 2; ch++)
                    {
                        mp2_idct36(sample_[ch], p, V_[ch], vpos_);
                        int64_t U[32] = {};
                        //  The window is doubled and the DCT output kept
                        //  whole, so the sums are what pl_mpeg's float ones
                        //  are, times two.
                        int d = 512 - (vpos_ >> 1);
                        int v = (vpos_ % 128) >> 1;
                        while (v < 1024)
                        {
                            for (int i = 0; i < 32; ++i)
                                U[i] += (int64_t)MP2_WINDOW[(d++) & 511] * V_[ch][v++];
                            v += 128 - 32;
                            d += 64 - 32;
                        }
                        d -= (512 - 32);
                        v = (128 - 32 + 1024) - v;
                        while (v < 1024)
                        {
                            for (int i = 0; i < 32; ++i)
                                U[i] += (int64_t)MP2_WINDOW[(d++) & 511] * V_[ch][v++];
                            v += 128 - 32;
                            d += 64 - 32;
                        }
                        //  pl_mpeg: U / -1090519040 as a float in (-1, 1).
                        //  Here: U (doubled) * 32768 / (2 * -1090519040), as
                        //  a multiply by -2^32 / 66560 and a shift.
                        for (int j = 0; j < 32; j++)
                        {
                            int64_t s = ((U[j] >> PRE_SHIFT) * -64528) >> 32;
                            out_[((outPos + j) << 1) + ch] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
                        }
                    }
                    outPos += 32;
                }
            }
        plm_buffer_align(buffer_);
    }
};
