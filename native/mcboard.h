// Model:Cycles virtual board: boots Elektron's full MAIN OS 1.13 on an emulated MCF54415.
//
// Driver-level shortcuts where the OS allows it (audio codec, screen bus, flash/eMMC timing), small
// register models where the OS needs real answers (interrupt controllers, timers, eSDHC + eMMC, eDMA,
// GPIO, SPI flash). Firmware and storage come from the user's own files; nothing is bundled.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mcboard mcboard;

// main_os: decompressed section 3 of the user's .syx. flash_path: 2 MB SPI-flash image (created with the
// firmware container at 0x20000). emmc_path: eMMC disk image (created sparse if missing).
mcboard *mcb_create(const uint8_t *main_os, size_t len, const char *flash_path, const char *emmc_path,
                    char *err, size_t errlen);
void mcb_destroy(mcboard *b);

// Runs the board on the calling thread for `seconds` of real time (0 = until mcb_stop).
// Returns 0, or nonzero if the emulated CPU faulted (see mcb_status).
int mcb_run(mcboard *b, double seconds);
void mcb_stop(mcboard *b);                    // thread-safe

// Latest 128x64 screen: column-major, pixel (x, y) = bit 7-(y&7) of byte x*8 + y/8, y = 0 at the bottom.
// Thread-safe snapshot, refreshed at ~60 Hz while running. Returns a counter that increases on change.
uint64_t mcb_screen(mcboard *b, uint8_t out[1024]);

// Front panel input (thread-safe). Keys are key-matrix indexes (OS key-code table 0x4010aef8):
//   0 PRESET MENU, 1 QUANTIZE, 2 CLICK, 3 LFO SETUP, 4 TAP BPM, 5 STOP, 6 TEMP SAVE, 7 PLAY, 8 PAD MENU,
//   9 RECORD, 10 FUNC, 11 PATTERN, 12 RETRIG, 13 TRACK, 14..29 steps 1..16, 30 PAGE.
// Knobs are the OS's
// encoder numbers 1..16 (1 LEVEL/DATA, 2 PITCH, 3 DECAY, 4 COLOR, 5 SHAPE, 6 SWEEP, 7 CONTOUR, 8 DELAY SEND,
// 9 REVERB SEND, 10 LFO SPEED, 11 VOL+DIST, 12 SWING, 13 CHANCE, 14 REVERB SIZE, 15 DELAY TIME, 16 MAIN VOLUME);
// a step is one detent (a full quadrature cycle, two OS knob events). Pads 1..6 are T1..T6, pressure 0..1.
void mcb_key(mcboard *b, int index, int down);
void mcb_encoder(mcboard *b, int number, int steps);
void mcb_pad(mcboard *b, int pad, float pressure);

// LEDs: bit n set = LED bit n lit (the OS's 7 latch bytes, 56 bits). Thread-safe snapshot.
uint64_t mcb_leds(mcboard *b);

// Audio: the OS's stereo output at 48 kHz (interleaved L,R floats). Reads up to `frames` frames, returns how
// many were available. Thread-safe for one reader (the audio callback) against the CPU thread.
size_t mcb_audio(mcboard *b, float *out, size_t frames);
size_t mcb_audio_available(mcboard *b);

// MIDI (the DIN/TRS ports): bytes in for the OS's MIDI IN, delivered at 31250 baud; bytes the OS sent on
// MIDI OUT/THRU. One writer and one reader each, thread-safe against the CPU thread.
void mcb_midi_in(mcboard *b, const uint8_t *data, size_t n);
size_t mcb_midi_out(mcboard *b, uint8_t *out, size_t max);

// USB: the board is plugged into a built-in host (set MCB_NOUSB to leave the cable out), which enumerates the
// OS's USB device and talks USB-MIDI to it. Raw MIDI bytes in/out, as for the DIN/TRS port above.
// mcb_usb_state: 0 not connected, 1 enumerating, 2 configured, -1 the device stopped answering.
void mcb_usb_midi_in(mcboard *b, const uint8_t *data, size_t n);
size_t mcb_usb_midi_out(mcboard *b, uint8_t *out, size_t max);
int mcb_usb_state(mcboard *b);

// USB audio (as a class-compliant 48 kHz stereo interface would stream it), each direction on when asked:
// capture = what the Model:Cycles sends to the computer, playback = what the computer sends to the Model:Cycles.
// Interleaved L,R floats; one reader (capture) and one writer (playback), thread-safe against the CPU thread.
void mcb_usb_audio_enable(mcboard *b, int capture, int playback);
size_t mcb_usb_audio_read(mcboard *b, float *out, size_t frames);
size_t mcb_usb_audio_read_available(mcboard *b);
size_t mcb_usb_audio_write(mcboard *b, const float *in, size_t frames);
size_t mcb_usb_audio_write_queued(mcboard *b);

// One-line status / diagnostics (fault, uptime, interrupts, storage activity).
void mcb_status(mcboard *b, char *out, size_t len);

#ifdef __cplusplus
}
#endif
