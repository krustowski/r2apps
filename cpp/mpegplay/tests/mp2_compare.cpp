//
//  mp2_compare --- mp2.hpp against pl_mpeg's own (float) MP2 decoder.
//
//      g++ -O2 -std=c++17 -I.. mp2_compare.cpp -o mp2_compare && ./mp2_compare a.mp2 ...
//
//  Decodes each file with both and prints the signal-to-noise ratio of the
//  integer decoder's output against the float one's; fails below 70 dB.
//
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PL_MPEG_IMPLEMENTATION
#include "../third_party/pl_mpeg.h"
#include "../mp2.hpp"

int main(int argc, char **argv)
{
    int bad = 0;
    for (int a = 1; a < argc; a++)
    {
        FILE *f = fopen(argv[a], "rb");
        if (!f)
            return 2;
        static uint8_t data[16 << 20];
        size_t n = fread(data, 1, sizeof(data), f);
        fclose(f);

        plm_audio_t *ref = plm_audio_create_with_buffer(plm_buffer_create_with_memory(data, n, 0), 1);
        Mp2Decoder mine(plm_buffer_create_with_memory(data, n, 0));
        double sig = 0, noise = 0;
        long frames = 0, peak = 0;
        plm_samples_t *s;
        while ((s = plm_audio_decode(ref)))
        {
            if (!mine.decode())
            {
                printf("%s: the integer decoder stopped at frame %ld\n", argv[a], frames);
                bad++;
                break;
            }
            for (int i = 0; i < PLM_AUDIO_SAMPLES_PER_FRAME * 2; i++)
            {
                double r = s->interleaved[i] * 32768.0;
                if (r > 32767) r = 32767;
                if (r < -32768) r = -32768;
                double d = r - mine.out()[i];
                sig += r * r;
                noise += d * d;
                if (labs(mine.out()[i]) > peak)
                    peak = labs(mine.out()[i]);
            }
            frames++;
        }
        double snr = noise > 0 ? 10 * log10(sig / noise) : 999;
        printf("%s: %ld frames at %d Hz, peak %ld, SNR %.1f dB\n", argv[a], frames, mine.rate(), peak, snr);
        if (snr < 70 || frames == 0)
            bad++;
    }
    return bad ? 1 : 0;
}
