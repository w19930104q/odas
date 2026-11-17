# STFT/ISTFT streaming-contract tests

Synthetic multichannel impulse tests for the STFT/ISTFT windowing,
tail-flush and restart contract. Both are designed to run under
AddressSanitizer (`-fsanitize=address`).

- `mod_stft_istft_test.c` — module-level chain. Covers full frames,
  a stream shorter than one frame (residual tail), two start/stop
  cycles (restart must reset the window state), and inputs with more
  channels than the module was built for. Asserts output sample
  counts, energy conservation, impulse alignment and restart
  determinism.
- `amod_stft_istft_test.c` — threaded chain (message pools + module
  threads, as in the real pipeline). Asserts the tail flush does not
  deadlock the threads and delivers the expected number of hops before
  the terminating empty message.

Build (from the repository root, on a system with gcc and fftw3):

```sh
gcc -Iinclude/odas -g -O1 -fsanitize=address -o /tmp/mod_stft_istft_test \
    src/module/mod_stft.c src/module/mod_istft.c \
    src/system/hop2frame.c src/system/frame2hop.c \
    src/system/frame2freq.c src/system/freq2frame.c \
    src/signal/hop.c src/signal/frame.c src/signal/freq.c src/signal/window.c \
    src/message/msg_hops.c src/message/msg_spectra.c \
    src/init/windowing.c src/utils/fft.c \
    test/mod_stft_istft_test.c -lfftw3f -lm

gcc -Iinclude/odas -g -O1 -fsanitize=address -o /tmp/amod_stft_istft_test \
    src/module/mod_stft.c src/module/mod_istft.c \
    src/amodule/amod_stft.c src/amodule/amod_istft.c \
    src/amessage/amsg_hops.c src/amessage/amsg_spectra.c \
    src/system/hop2frame.c src/system/frame2hop.c \
    src/system/frame2freq.c src/system/freq2frame.c \
    src/signal/hop.c src/signal/frame.c src/signal/freq.c src/signal/window.c \
    src/message/msg_hops.c src/message/msg_spectra.c \
    src/init/windowing.c src/utils/fft.c src/utils/fifo.c src/general/thread.c \
    test/amod_stft_istft_test.c -lfftw3f -lm -lpthread
```

Run:

```sh
/tmp/mod_stft_istft_test    # prints PASS/FAIL per assertion
/tmp/amod_stft_istft_test   # completes only if the threads terminate
```

Both exit 0 on success. The threaded test is best run under a timeout
(e.g. `timeout 60`) so a broken flush contract shows up as a hang
instead of blocking forever.
