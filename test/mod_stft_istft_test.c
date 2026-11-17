
   /**
    * \file     mod_stft_istft_test.c
    * \author   François Grondin <francois.grondin2@usherbrooke.ca>
    * \version  2.0
    * \date     2018-03-18
    * \copyright
    *
    * Permission is hereby granted, free of charge, to any person obtaining
    * a copy of this software and associated documentation files (the
    * "Software"), to deal in the Software without restriction, including
    * without limitation the rights to use, copy, modify, merge, publish,
    * distribute, sublicense, and/or sell copies of the Software, and to
    * permit persons to whom the Software is furnished to do so, subject to
    * the following conditions:
    *
    * The above copyright notice and this permission notice shall be
    * included in all copies or substantial portions of the Software.
    *
    * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
    * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
    * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
    * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
    * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
    *
    */

    // Synthetic multichannel impulse test for the STFT/ISTFT streaming
    // contract:
    //
    //  1. Full frames: input length is an exact multiple of the hop size.
    //  2. Residual tail: an impulse in the last hop leaves less than one
    //     frame in the sliding window; the tail must be flushed, not
    //     dropped.
    //  3. Two start/stop cycles: disable/enable between runs must reset
    //     the window state so both runs produce identical output.
    //  4. High channel count input: messages with more channels than the
    //     module was built for must not read or write out of bounds
    //     (checked with ASAN).
    //
    // Assertions: output sample counts, energy conservation, impulse
    // alignment, restart determinism.

    #include <math.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>

    #include <module/mod_stft.h>
    #include <module/mod_istft.h>

    #define HOP_SIZE        128
    #define HALF_FRAME_SIZE 129
    #define FRAME_SIZE      (2*(HALF_FRAME_SIZE-1))
    #define FS              16000

    // ceil((frameSize-hopSize)/hopSize) zero-padded hops drain the window
    #define N_FLUSH         (((FRAME_SIZE)-1)/(HOP_SIZE))

    // Reconstruction delay introduced by the analysis/synthesis chain
    #define DELAY           ((FRAME_SIZE)-(HOP_SIZE))

    #define MAX_HOPS        64

    static unsigned int nFailures = 0;

    static void check(const char * name, int condition) {

        printf("  [%s] %s\n", condition ? "PASS" : "FAIL", name);
        if (!condition) { nFailures++; }

    }

    typedef struct chain_obj {

        mod_stft_obj * stft;
        mod_istft_obj * istft;

        msg_hops_obj * in;
        msg_spectra_obj * mid;
        msg_hops_obj * out;

    } chain_obj;

    static chain_obj * chain_construct(unsigned int nChannelsIn, unsigned int nChannelsMid, unsigned int nChannelsOut) {

        chain_obj * chain;
        mod_stft_cfg * stftCfg;
        mod_istft_cfg * istftCfg;
        msg_hops_cfg * inCfg;
        msg_spectra_cfg * midCfg;
        msg_hops_cfg * outCfg;

        chain = (chain_obj *) malloc(sizeof(chain_obj));

        inCfg = msg_hops_cfg_construct();
        inCfg->hopSize = HOP_SIZE;
        inCfg->nChannels = nChannelsIn;
        inCfg->fS = FS;

        midCfg = msg_spectra_cfg_construct();
        midCfg->halfFrameSize = HALF_FRAME_SIZE;
        midCfg->nChannels = nChannelsMid;
        midCfg->fS = FS;

        outCfg = msg_hops_cfg_construct();
        outCfg->hopSize = HOP_SIZE;
        outCfg->nChannels = nChannelsOut;
        outCfg->fS = FS;

        stftCfg = mod_stft_cfg_construct();
        istftCfg = mod_istft_cfg_construct();

        chain->stft = mod_stft_construct(stftCfg, inCfg, midCfg);
        chain->istft = mod_istft_construct(istftCfg, midCfg, outCfg);

        chain->in = msg_hops_construct(inCfg);
        chain->mid = msg_spectra_construct(midCfg);
        chain->out = msg_hops_construct(outCfg);

        mod_stft_connect(chain->stft, chain->in, chain->mid);
        mod_istft_connect(chain->istft, chain->mid, chain->out);

        mod_stft_enable(chain->stft);
        mod_istft_enable(chain->istft);

        mod_stft_cfg_destroy(stftCfg);
        mod_istft_cfg_destroy(istftCfg);
        msg_hops_cfg_destroy(inCfg);
        msg_spectra_cfg_destroy(midCfg);
        msg_hops_cfg_destroy(outCfg);

        return chain;

    }

    static void chain_destroy(chain_obj * chain) {

        mod_stft_destroy(chain->stft);
        mod_istft_destroy(chain->istft);
        msg_hops_destroy(chain->in);
        msg_spectra_destroy(chain->mid);
        msg_hops_destroy(chain->out);
        free((void *) chain);

    }

    static void chain_restart(chain_obj * chain) {

        mod_stft_disable(chain->stft);
        mod_istft_disable(chain->istft);
        mod_stft_enable(chain->stft);
        mod_istft_enable(chain->istft);

    }

    // Push nHops of input through the STFT -> ISTFT chain, then push the
    // empty (end-of-stream) message and drain the tail. Returns the number
    // of output hops written to output (nChannelsOut x MAX_HOPS*HOP_SIZE).
    static unsigned int chain_run(chain_obj * chain,
                                  float ** input, unsigned int nChannelsIn, unsigned int nHops,
                                  float ** output, unsigned int nChannelsOut) {

        unsigned int iHop;
        unsigned int iChannel;
        unsigned int nOut;
        int rtnStft;
        int rtnIstft;
        unsigned int nIters;

        nOut = 0;

        for (iHop = 0; iHop < nHops; iHop++) {

            for (iChannel = 0; iChannel < nChannelsIn; iChannel++) {
                memcpy(&(chain->in->hops->array[iChannel][0]),
                       &(input[iChannel][iHop * HOP_SIZE]),
                       sizeof(float) * HOP_SIZE);
            }
            chain->in->timeStamp = (unsigned long long) (iHop + 1);

            rtnStft = mod_stft_process(chain->stft);
            rtnIstft = mod_istft_process(chain->istft);

            if ((rtnStft != 0) || (rtnIstft != 0)) {
                printf("  [FAIL] unexpected return while streaming (stft=%d istft=%d)\n", rtnStft, rtnIstft);
                nFailures++;
            }

            for (iChannel = 0; iChannel < nChannelsOut; iChannel++) {
                memcpy(&(output[iChannel][nOut * HOP_SIZE]),
                       &(chain->out->hops->array[iChannel][0]),
                       sizeof(float) * HOP_SIZE);
            }
            nOut++;

        }

        // End of stream: the empty message flushes the tail, then
        // terminates both modules
        msg_hops_zero(chain->in);

        nIters = 0;
        while (nIters++ < 100) {

            rtnStft = mod_stft_process(chain->stft);
            rtnIstft = mod_istft_process(chain->istft);

            if (rtnIstft == -1) { break; }

            for (iChannel = 0; iChannel < nChannelsOut; iChannel++) {
                memcpy(&(output[iChannel][nOut * HOP_SIZE]),
                       &(chain->out->hops->array[iChannel][0]),
                       sizeof(float) * HOP_SIZE);
            }
            nOut++;

        }

        if (rtnStft != -1) {
            printf("  [FAIL] stft did not terminate after the flush (rtn=%d)\n", rtnStft);
            nFailures++;
        }
        if (rtnIstft != -1) {
            printf("  [FAIL] istft did not terminate after the flush (rtn=%d)\n", rtnIstft);
            nFailures++;
        }

        return nOut;

    }

    static double energy(const float * signal, unsigned int nSamples) {

        double e;
        unsigned int iSample;

        e = 0.0;
        for (iSample = 0; iSample < nSamples; iSample++) {
            e += ((double) signal[iSample]) * ((double) signal[iSample]);
        }

        return e;

    }

    static void peak(const float * signal, unsigned int nSamples, unsigned int * pos, float * value) {

        unsigned int iSample;
        float best;

        best = 0.0f;
        *pos = 0;
        *value = 0.0f;

        for (iSample = 0; iSample < nSamples; iSample++) {
            if (fabsf(signal[iSample]) > best) {
                best = fabsf(signal[iSample]);
                *pos = iSample;
                *value = signal[iSample];
            }
        }

    }

    static void check_reconstruction(const char * label,
                                     float ** input, float ** output,
                                     unsigned int nChannels, unsigned int nHopsIn, unsigned int nHopsOut) {

        char name[128];
        unsigned int iChannel;
        unsigned int nInSamples;
        unsigned int nOutSamples;
        double eIn;
        double eOut;
        unsigned int pos;
        float value;
        unsigned int expectedPos;
        float expectedValue;

        nInSamples = nHopsIn * HOP_SIZE;
        nOutSamples = nHopsOut * HOP_SIZE;

        snprintf(name, sizeof(name), "%s: output sample count", label);
        check(name, nHopsOut == nHopsIn + 2 * N_FLUSH);

        eIn = 0.0;
        eOut = 0.0;
        for (iChannel = 0; iChannel < nChannels; iChannel++) {
            eIn += energy(input[iChannel], nInSamples);
            eOut += energy(output[iChannel], nOutSamples);
        }

        snprintf(name, sizeof(name), "%s: energy conserved (in=%.6f out=%.6f)", label, eIn, eOut);
        check(name, (eIn > 0.0) && (fabs(eOut - eIn) / eIn < 0.02));

        for (iChannel = 0; iChannel < nChannels; iChannel++) {

            peak(input[iChannel], nInSamples, &pos, &value);
            expectedPos = pos;
            expectedValue = value;

            peak(output[iChannel], nOutSamples, &pos, &value);

            snprintf(name, sizeof(name), "%s: ch%u impulse aligned (pos=%u expected=%u)",
                     label, iChannel, pos, expectedPos + DELAY);
            check(name, pos == expectedPos + DELAY);

            snprintf(name, sizeof(name), "%s: ch%u impulse amplitude (got=%.4f expected=%.4f)",
                     label, iChannel, value, expectedValue);
            check(name, fabsf(value - expectedValue) < 0.05f);

        }

    }

    static float ** signals_alloc(unsigned int nChannels, unsigned int nSamples) {

        float ** signals;
        unsigned int iChannel;

        signals = (float **) malloc(sizeof(float *) * nChannels);
        for (iChannel = 0; iChannel < nChannels; iChannel++) {
            signals[iChannel] = (float *) malloc(sizeof(float) * nSamples);
            memset(signals[iChannel], 0x00, sizeof(float) * nSamples);
        }

        return signals;

    }

    static void signals_free(float ** signals, unsigned int nChannels) {

        unsigned int iChannel;

        for (iChannel = 0; iChannel < nChannels; iChannel++) {
            free((void *) signals[iChannel]);
        }
        free((void *) signals);

    }

    // Scenario 1 & 3: full frames (input is an exact multiple of the hop
    // size), an impulse in the last hop (residual tail), and two
    // start/stop cycles that must produce identical output.
    static void test_full_frames_and_restart(void) {

        const unsigned int nChannels = 4;
        const unsigned int nHops = 16;
        const unsigned int nSamples = nHops * HOP_SIZE;
        const float amps[4] = { 1.0f, -0.5f, 0.25f, 2.0f };
        const unsigned int poss[4] = { 100, 1000, 2047, 1500 };

        chain_obj * chain;
        float ** input;
        float ** output1;
        float ** output2;
        unsigned int nOut1;
        unsigned int nOut2;
        unsigned int iChannel;
        unsigned int iSample;
        float maxDiff;

        printf("Scenario 1: full frames + residual tail + restart\n");

        input = signals_alloc(nChannels, nSamples);
        output1 = signals_alloc(nChannels, MAX_HOPS * HOP_SIZE);
        output2 = signals_alloc(nChannels, MAX_HOPS * HOP_SIZE);

        for (iChannel = 0; iChannel < nChannels; iChannel++) {
            input[iChannel][poss[iChannel]] = amps[iChannel];
        }

        chain = chain_construct(nChannels, nChannels, nChannels);

        nOut1 = chain_run(chain, input, nChannels, nHops, output1, nChannels);

        check_reconstruction("run1", input, output1, nChannels, nHops, nOut1);

        // Second start/stop cycle with the very same input
        chain_restart(chain);

        nOut2 = chain_run(chain, input, nChannels, nHops, output2, nChannels);

        check("run2: same output hop count as run1", nOut2 == nOut1);

        maxDiff = 0.0f;
        for (iChannel = 0; iChannel < nChannels; iChannel++) {
            for (iSample = 0; iSample < nOut1 * HOP_SIZE; iSample++) {
                float diff = fabsf(output1[iChannel][iSample] - output2[iChannel][iSample]);
                if (diff > maxDiff) { maxDiff = diff; }
            }
        }

        printf("  [info] max |run1-run2| = %g\n", maxDiff);
        check("run2: restart resets the window state (identical output)", maxDiff < 1e-6f);

        chain_destroy(chain);
        signals_free(input, nChannels);
        signals_free(output1, nChannels);
        signals_free(output2, nChannels);

    }

    // Scenario 2: the whole stream is shorter than one frame; the tail
    // left in the sliding window must still be flushed.
    static void test_residual_frame(void) {

        const unsigned int nChannels = 2;
        const unsigned int nHops = 1;
        const unsigned int nSamples = nHops * HOP_SIZE;

        chain_obj * chain;
        float ** input;
        float ** output;
        unsigned int nOut;

        printf("Scenario 2: stream shorter than one frame\n");

        input = signals_alloc(nChannels, nSamples);
        output = signals_alloc(nChannels, MAX_HOPS * HOP_SIZE);

        input[0][100] = 1.0f;
        input[1][127] = -1.0f;

        chain = chain_construct(nChannels, nChannels, nChannels);

        nOut = chain_run(chain, input, nChannels, nHops, output, nChannels);

        check_reconstruction("short", input, output, nChannels, nHops, nOut);

        chain_destroy(chain);
        signals_free(input, nChannels);
        signals_free(output, nChannels);

    }

    // Scenario 4: messages with more channels than the module was built
    // for must not read or write out of bounds.
    static void test_high_channel_count(void) {

        const unsigned int nHops = 4;
        const unsigned int nSamples = nHops * HOP_SIZE;

        chain_obj * chain;
        float ** input;
        float ** output;
        unsigned int nOut;
        unsigned int iChannel;
        unsigned int iSample;
        double eIn;
        double eOut;
        float maxLeak;

        printf("Scenario 4: high channel count input\n");

        // 4a: 8-channel hops into a chain built for 4 channels; the first
        // 4 channels must be reconstructed, nothing may overflow
        {
            const unsigned int nChannelsIn = 8;
            const unsigned int nChannels = 4;

            input = signals_alloc(nChannelsIn, nSamples);
            output = signals_alloc(nChannels, MAX_HOPS * HOP_SIZE);

            for (iChannel = 0; iChannel < nChannelsIn; iChannel++) {
                input[iChannel][37 + iChannel * 61] = 0.5f;
            }

            chain = chain_construct(nChannelsIn, nChannels, nChannels);
            nOut = chain_run(chain, input, nChannelsIn, nHops, output, nChannels);

            check("wide-in: output sample count", nOut == nHops + 2 * N_FLUSH);

            eIn = 0.0;
            eOut = 0.0;
            for (iChannel = 0; iChannel < nChannels; iChannel++) {
                eIn += energy(input[iChannel], nSamples);
                eOut += energy(output[iChannel], nOut * HOP_SIZE);
            }
            printf("  [info] wide-in energy in=%.6f out=%.6f\n", eIn, eOut);
            check("wide-in: energy of the kept channels conserved",
                  (eIn > 0.0) && (fabs(eOut - eIn) / eIn < 0.02));

            chain_destroy(chain);
            signals_free(input, nChannelsIn);
            signals_free(output, nChannels);
        }

        // 4b: 4-channel spectra into a chain built for 8 channels; the
        // extra output channels must be zero, nothing may overflow
        {
            const unsigned int nChannelsMid = 4;
            const unsigned int nChannels = 8;

            input = signals_alloc(nChannels, nSamples);
            output = signals_alloc(nChannels, MAX_HOPS * HOP_SIZE);

            for (iChannel = 0; iChannel < nChannelsMid; iChannel++) {
                input[iChannel][53 + iChannel * 47] = 0.75f;
            }

            chain = chain_construct(nChannels, nChannelsMid, nChannels);
            nOut = chain_run(chain, input, nChannels, nHops, output, nChannels);

            check("narrow-mid: output sample count", nOut == nHops + 2 * N_FLUSH);

            eIn = 0.0;
            eOut = 0.0;
            for (iChannel = 0; iChannel < nChannelsMid; iChannel++) {
                eIn += energy(input[iChannel], nSamples);
                eOut += energy(output[iChannel], nOut * HOP_SIZE);
            }
            printf("  [info] narrow-mid energy in=%.6f out=%.6f\n", eIn, eOut);
            check("narrow-mid: energy of the kept channels conserved",
                  (eIn > 0.0) && (fabs(eOut - eIn) / eIn < 0.02));

            maxLeak = 0.0f;
            for (iChannel = nChannelsMid; iChannel < nChannels; iChannel++) {
                for (iSample = 0; iSample < nOut * HOP_SIZE; iSample++) {
                    float a = fabsf(output[iChannel][iSample]);
                    if (a > maxLeak) { maxLeak = a; }
                }
            }
            check("narrow-mid: unmatched channels stay zero", maxLeak == 0.0f);

            chain_destroy(chain);
            signals_free(input, nChannels);
            signals_free(output, nChannels);
        }

    }

    // Scenario 5: the windowing units themselves must never read or write
    // past their own buffers when the messages carry more channels than
    // the unit was built for (direct unit-level check of the guards).
    static void test_unit_channel_bounds(void) {

        hop2frame_obj * hop2frame;
        frame2hop_obj * frame2hop;
        hops_obj * hopsWide;
        hops_obj * hopsNarrow;
        frames_obj * framesNarrow;
        frames_obj * framesWide;
        unsigned int iChannel;
        unsigned int iSample;
        float maxAbs;

        printf("Scenario 5: unit-level channel bounds\n");

        // 5a: 8-channel hops into a 4-channel hop2frame
        hop2frame = hop2frame_construct_zero(HOP_SIZE, FRAME_SIZE, 4);
        hopsWide = hops_construct_zero(8, HOP_SIZE);
        framesNarrow = frames_construct_zero(4, FRAME_SIZE);

        for (iChannel = 0; iChannel < 8; iChannel++) {
            for (iSample = 0; iSample < HOP_SIZE; iSample++) {
                hopsWide->array[iChannel][iSample] = 1.0f;
            }
        }

        hop2frame_process(hop2frame, hopsWide, framesNarrow);
        hop2frame_flush(hop2frame, framesNarrow);

        maxAbs = 0.0f;
        for (iChannel = 0; iChannel < 4; iChannel++) {
            for (iSample = 0; iSample < FRAME_SIZE; iSample++) {
                float a = fabsf(framesNarrow->array[iChannel][iSample]);
                if (a > maxAbs) { maxAbs = a; }
            }
        }
        check("hop2frame: wide input bounded, output sane", maxAbs <= 1.0f);

        hop2frame_destroy(hop2frame);
        hops_destroy(hopsWide);
        frames_destroy(framesNarrow);

        // 5b: 4-channel frames into an 8-channel frame2hop
        frame2hop = frame2hop_construct_zero(HOP_SIZE, FRAME_SIZE, 8);
        framesWide = frames_construct_zero(4, FRAME_SIZE);
        hopsNarrow = hops_construct_zero(8, HOP_SIZE);

        for (iChannel = 0; iChannel < 4; iChannel++) {
            for (iSample = 0; iSample < FRAME_SIZE; iSample++) {
                framesWide->array[iChannel][iSample] = 1.0f;
            }
        }

        frame2hop_process(frame2hop, framesWide, hopsNarrow);
        frame2hop_flush(frame2hop, hopsNarrow);

        maxAbs = 0.0f;
        for (iChannel = 4; iChannel < 8; iChannel++) {
            for (iSample = 0; iSample < HOP_SIZE; iSample++) {
                float a = fabsf(hopsNarrow->array[iChannel][iSample]);
                if (a > maxAbs) { maxAbs = a; }
            }
        }
        check("frame2hop: narrow input bounded, unmatched hops zero", maxAbs == 0.0f);

        frame2hop_destroy(frame2hop);
        frames_destroy(framesWide);
        hops_destroy(hopsNarrow);

    }

    int main(int argc, char ** argv) {

        (void) argc;
        (void) argv;

        // Keep results visible even if ASAN aborts the process
        setvbuf(stdout, NULL, _IONBF, 0);

        test_full_frames_and_restart();
        test_residual_frame();
        test_high_channel_count();
        test_unit_channel_bounds();

        if (nFailures == 0) {
            printf("ALL TESTS PASSED\n");
            return 0;
        }

        printf("%u TEST(S) FAILED\n", nFailures);
        return 1;

    }
