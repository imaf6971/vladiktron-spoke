// Headless MIDI check: boots, then (1) sends notes on channels 1..16 to MIDI IN and measures the audio each one
// makes, (2) plays pads and PLAY and prints what the OS sends on MIDI OUT (nothing with factory MIDI settings),
// (3) checks the USB link (enumerated, USB-MIDI notes play).
//   mcmidi_test <main_os.bin> <flash.bin> <emmc.img>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "mcboard.h"

static double level(mcboard *b, double secs) {       // peak of the output over `secs`
    float tmp[2 * 4096];
    double peak = 0;
    for (double t = 0; t < secs; t += 0.02) {
        mcb_run(b, 0.02);
        size_t k;
        while ((k = mcb_audio(b, tmp, 4096)) > 0)
            for (size_t i = 0; i < 2 * k; i++) if (fabs(tmp[i]) > peak) peak = fabs(tmp[i]);
    }
    return peak;
}

static void dump_out(mcboard *b, const char *what) {
    uint8_t buf[4096];
    size_t n = mcb_midi_out(b, buf, sizeof buf), shown = 0;
    printf("%-14s %4zu bytes out:", what, n);
    for (size_t i = 0; i < n && shown < 24; i++) {
        if (buf[i] == 0xF8) continue;                 // clock: counted, not listed
        printf(" %02x", buf[i]); shown++;
    }
    size_t clocks = 0;
    for (size_t i = 0; i < n; i++) clocks += buf[i] == 0xF8;
    printf("%s  (clock bytes: %zu)\n", shown == 24 ? " ..." : "", clocks);
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *os = malloc(n); if (fread(os, 1, n, f) != n) return 1; fclose(f);
    char err[256], st[512];
    mcboard *b = mcb_create(os, n, argv[2], argv[3], err, sizeof err);
    if (!b) { fprintf(stderr, "%s\n", err); return 1; }
    mcb_run(b, 6);                                    // boot
    dump_out(b, "boot");

    printf("MIDI IN, note C4 vel 100 on each channel (peak level; silence = %.4f):\n", level(b, 0.3));
    for (int ch = 0; ch < 16; ch++) {
        uint8_t on[3] = {(uint8_t)(0x90 | ch), 60, 100}, off[3] = {(uint8_t)(0x80 | ch), 60, 0};
        mcb_midi_in(b, on, 3);
        double p = level(b, 0.25);
        mcb_midi_in(b, off, 3);
        level(b, 0.6);                                // let it decay
        printf("  ch %2d: %.4f\n", ch + 1, p);
    }
    dump_out(b, "after notes");

    for (int pad = 1; pad <= 6; pad++) {
        mcb_pad(b, pad, 0.8); mcb_run(b, 0.1); mcb_pad(b, pad, 0); mcb_run(b, 0.2);
    }
    dump_out(b, "pads 1-6");
    mcb_key(b, 7, 1); mcb_run(b, 0.1); mcb_key(b, 7, 0); mcb_run(b, 1.0);
    dump_out(b, "PLAY 1 s");
    mcb_key(b, 7, 1); mcb_run(b, 0.1); mcb_key(b, 7, 0); mcb_run(b, 0.3);
    dump_out(b, "PLAY again");

    printf("USB: state %d (2 = configured)\n", mcb_usb_state(b));
    mcb_key(b, 5, 1); mcb_run(b, 0.1); mcb_key(b, 5, 0); level(b, 1.0);          // STOP, let it ring out
    uint8_t usb_on[3] = {0x91, 62, 110}, usb_off[3] = {0x81, 62, 0};
    mcb_usb_midi_in(b, usb_on, 3);
    printf("  USB-MIDI note on channel 2: peak %.4f\n", level(b, 0.25));
    mcb_usb_midi_in(b, usb_off, 3);
    uint8_t ubuf[256];
    size_t un = mcb_usb_midi_out(b, ubuf, sizeof ubuf);
    printf("  USB-MIDI from the OS: %zu bytes\n", un);

    mcb_status(b, st, sizeof st);
    printf("%s\n", st);
    mcb_destroy(b);
    return 0;
}
