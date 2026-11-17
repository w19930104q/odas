
   /**
    * \file     mod_stft.c
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

    #include <module/mod_stft.h>

    mod_stft_obj * mod_stft_construct(const mod_stft_cfg * mod_stft_config, const msg_hops_cfg * msg_hops_config, const msg_spectra_cfg * msg_spectra_config) {

        mod_stft_obj * obj;
        unsigned int frameSize;

        obj = (mod_stft_obj *) malloc(sizeof(mod_stft_obj));

        frameSize = 2*(msg_spectra_config->halfFrameSize-1);

        obj->hop2frame =  hop2frame_construct_zero(msg_hops_config->hopSize, frameSize, msg_hops_config->nChannels);
        obj->frames = frames_construct_zero(msg_hops_config->nChannels,frameSize);
        obj->frame2freq = frame2freq_construct_zero(frameSize, msg_spectra_config->halfFrameSize);

        obj->in = (msg_hops_obj *) NULL;
        obj->out = (msg_spectra_obj *) NULL;

        obj->enabled = 0;

        // Number of zero-padded hops to push so the tail samples left in
        // the sliding window are flushed out: ceil((frameSize-hopSize)/hopSize)
        obj->nFlushes = 0;
        if ((msg_hops_config->hopSize > 0) && (frameSize > msg_hops_config->hopSize)) {
            obj->nFlushes = (frameSize - 1) / msg_hops_config->hopSize;
        }
        obj->flushCount = 0;
        obj->timeStamp = 0;

        return obj;

    }

    void mod_stft_destroy(mod_stft_obj * obj) {

        hop2frame_destroy(obj->hop2frame);
        frames_destroy(obj->frames);
        frame2freq_destroy(obj->frame2freq);

        free((void *) obj);

    }

    int mod_stft_process(mod_stft_obj * obj) {

        int rtnValue;

        if (msg_hops_isZero(obj->in) == 0) {

            if (obj->enabled == 1) {

                hop2frame_process(obj->hop2frame, obj->in->hops, obj->frames);
                frame2freq_process(obj->frame2freq, obj->frames, obj->out->freqs);

            }
            else {

                freqs_zero(obj->out->freqs);

            }

            obj->out->timeStamp = obj->in->timeStamp;
            obj->timeStamp = obj->in->timeStamp;

            rtnValue = 0;

        }
        else {

            if ((obj->enabled == 1) && (obj->flushCount < obj->nFlushes)) {

                // Push zero hops to drain the tail samples left in the
                // sliding window before the stream terminates
                hop2frame_flush(obj->hop2frame, obj->frames);
                frame2freq_process(obj->frame2freq, obj->frames, obj->out->freqs);

                obj->timeStamp += (unsigned long long) obj->hop2frame->hopSize;
                obj->out->timeStamp = obj->timeStamp;

                obj->flushCount++;

                rtnValue = 1;

            }
            else {

                msg_spectra_zero(obj->out);

                rtnValue = -1;

            }

        }

        return rtnValue;

    }

    void mod_stft_connect(mod_stft_obj * obj, msg_hops_obj * in, msg_spectra_obj * out) {

        obj->in = in;
        obj->out = out;

    }

    void mod_stft_disconnect(mod_stft_obj * obj) {

        obj->in = (msg_hops_obj *) NULL;
        obj->out = (msg_spectra_obj *) NULL;

    }

    void mod_stft_enable(mod_stft_obj * obj) {

        // Restarting the module resets the sliding window and the flush
        // state, so the stream restarts from a clean state
        hop2frame_zero(obj->hop2frame);
        obj->flushCount = 0;
        obj->timeStamp = 0;

        obj->enabled = 1;

    }

    void mod_stft_disable(mod_stft_obj * obj) {

        obj->enabled = 0;

    }

    mod_stft_cfg * mod_stft_cfg_construct(void) {

        mod_stft_cfg * cfg;

        cfg = (mod_stft_cfg *) malloc(sizeof(mod_stft_cfg));

        return cfg;

    }

    void mod_stft_cfg_destroy(mod_stft_cfg * cfg) {

        free((void *) cfg);

    }


    void mod_stft_cfg_printf(const mod_stft_cfg * cfg) {

    }
