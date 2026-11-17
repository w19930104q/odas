
   /**
    * \file     amod_stft_istft_test.c
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

    // Threaded integration test for the STFT -> ISTFT chain: the modules
    // run in their own threads and exchange messages through pools, as in
    // the real pipeline. A single empty message terminates the stream;
    // the tail flush must not deadlock the threads and must deliver the
    // expected number of hops before the terminating empty message.

    #include <math.h>
    #include <pthread.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>

    #include <amodule/amod_stft.h>
    #include <amodule/amod_istft.h>

    #define HOP_SIZE        128
    #define HALF_FRAME_SIZE 129
    #define FRAME_SIZE      (2*(HALF_FRAME_SIZE-1))
    #define N_CHANNELS      4
    #define N_MESSAGES      4
    #define N_HOPS          16
    #define FS              16000

    #define N_FLUSH         (((FRAME_SIZE)-1)/(HOP_SIZE))

    static unsigned int nFailures = 0;

    static void check(const char * name, int condition) {

        printf("  [%s] %s\n", condition ? "PASS" : "FAIL", name);
        if (!condition) { nFailures++; }

    }

    // Producer: synthetic multichannel impulses, then a single empty
    // message to end the stream (runs in its own thread, like asrc_hops)
    static void * produce(void * ptr) {

        amsg_hops_obj * amsg_in;
        msg_hops_obj * msg;
        unsigned int iHop;
        unsigned int iChannel;

        amsg_in = (amsg_hops_obj *) ptr;

        for (iHop = 0; iHop < N_HOPS; iHop++) {

            msg = amsg_hops_empty_pop(amsg_in);

            for (iChannel = 0; iChannel < N_CHANNELS; iChannel++) {
                memset(msg->hops->array[iChannel], 0x00, sizeof(float) * HOP_SIZE);
            }
            if ((iHop % 5) == 0) {
                for (iChannel = 0; iChannel < N_CHANNELS; iChannel++) {
                    msg->hops->array[iChannel][(iHop * 13 + iChannel * 29) % HOP_SIZE] = 0.5f;
                }
            }
            msg->timeStamp = (unsigned long long) (iHop + 1);

            amsg_hops_filled_push(amsg_in, msg);

        }

        msg = amsg_hops_empty_pop(amsg_in);
        msg_hops_zero(msg);
        amsg_hops_filled_push(amsg_in, msg);

        return NULL;

    }

    int main(int argc, char ** argv) {

        mod_stft_cfg * stftCfg;
        mod_istft_cfg * istftCfg;
        msg_hops_cfg * hopsCfg;
        msg_spectra_cfg * spectraCfg;

        amsg_hops_obj * amsg_in;
        amsg_spectra_obj * amsg_mid;
        amsg_hops_obj * amsg_out;

        amod_stft_obj * amod_stft;
        amod_istft_obj * amod_istft;

        msg_hops_obj * msg;
        pthread_t producer;
        unsigned int iChannel;
        unsigned int iSample;
        unsigned int nOut;
        double energyIn;
        double energyOut;

        (void) argc;
        (void) argv;

        setvbuf(stdout, NULL, _IONBF, 0);

        printf("Scenario 6: threaded chain, flush and termination\n");

        hopsCfg = msg_hops_cfg_construct();
        hopsCfg->hopSize = HOP_SIZE;
        hopsCfg->nChannels = N_CHANNELS;
        hopsCfg->fS = FS;

        spectraCfg = msg_spectra_cfg_construct();
        spectraCfg->halfFrameSize = HALF_FRAME_SIZE;
        spectraCfg->nChannels = N_CHANNELS;
        spectraCfg->fS = FS;

        amsg_in = amsg_hops_construct(N_MESSAGES, hopsCfg);
        amsg_mid = amsg_spectra_construct(N_MESSAGES, spectraCfg);
        amsg_out = amsg_hops_construct(N_MESSAGES, hopsCfg);

        stftCfg = mod_stft_cfg_construct();
        istftCfg = mod_istft_cfg_construct();

        amod_stft = amod_stft_construct(stftCfg, hopsCfg, spectraCfg);
        amod_istft = amod_istft_construct(istftCfg, spectraCfg, hopsCfg);

        amod_stft_connect(amod_stft, amsg_in, amsg_mid);
        amod_istft_connect(amod_istft, amsg_mid, amsg_out);

        amod_stft_enable(amod_stft);
        amod_istft_enable(amod_istft);

        thread_start(amod_stft->thread);
        thread_start(amod_istft->thread);

        // Producer runs in its own thread, like asrc_hops in the real
        // pipeline
        energyIn = 0.0;
        {
            unsigned int iHop;
            for (iHop = 0; iHop < N_HOPS; iHop++) {
                if ((iHop % 5) == 0) {
                    energyIn += 0.25 * N_CHANNELS;
                }
            }
        }
        pthread_create(&producer, NULL, &produce, (void *) amsg_in);

        // Consumer: count hops and accumulate energy until the empty
        // message comes back
        nOut = 0;
        energyOut = 0.0;
        while (1) {

            msg = amsg_hops_filled_pop(amsg_out);

            if (msg_hops_isZero(msg) != 0) {
                amsg_hops_empty_push(amsg_out, msg);
                break;
            }

            for (iChannel = 0; iChannel < N_CHANNELS; iChannel++) {
                for (iSample = 0; iSample < HOP_SIZE; iSample++) {
                    double s = (double) msg->hops->array[iChannel][iSample];
                    energyOut += s * s;
                }
            }
            nOut++;

            amsg_hops_empty_push(amsg_out, msg);

        }

        // Both module threads must have terminated after forwarding the
        // empty message (a broken flush contract would hang the join)
        pthread_join(producer, NULL);
        thread_join(amod_stft->thread);
        thread_join(amod_istft->thread);

        check("threads terminated after the flush", 1);

        check("output hop count (streamed + flushed tail)",
              nOut == N_HOPS + 2 * N_FLUSH);

        printf("  [info] energy in=%.6f out=%.6f\n", energyIn, energyOut);
        check("energy conserved through the threaded chain",
              (energyIn > 0.0) && (fabs(energyOut - energyIn) / energyIn < 0.02));

        amod_stft_destroy(amod_stft);
        amod_istft_destroy(amod_istft);

        amsg_hops_destroy(amsg_in);
        amsg_spectra_destroy(amsg_mid);
        amsg_hops_destroy(amsg_out);

        mod_stft_cfg_destroy(stftCfg);
        mod_istft_cfg_destroy(istftCfg);
        msg_hops_cfg_destroy(hopsCfg);
        msg_spectra_cfg_destroy(spectraCfg);

        if (nFailures == 0) {
            printf("ALL TESTS PASSED\n");
            return 0;
        }

        printf("%u TEST(S) FAILED\n", nFailures);
        return 1;

    }
