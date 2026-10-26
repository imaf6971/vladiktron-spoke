// Model:Cycles OS 1.13 voice engine, run in an emulated ColdFire (patched Unicorn).
// C port of Modded-Cycles tools/emu/mcengine.py. The OS image (section 3, "MAIN OS") is read from the
// user's own .syx at run time; nothing from the firmware is bundled.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define MC_TRACKS 6
#define MC_FRAMES 32          // frames per block; 1500 blocks = 1 s at 48 kHz
#define MC_RATE 48000

typedef enum { MC_KICK, MC_SNARE, MC_METAL, MC_PERC, MC_TONE, MC_CHORD } mc_machine;

typedef enum {
    MC_P_MACHINE, MC_P_PITCH, MC_P_COLOR, MC_P_SHAPE, MC_P_SWEEP, MC_P_CONTOUR,
    MC_P_PUNCH, MC_P_GATE, MC_P_FINETUNE, MC_P_DECAY, MC_P_NOTE, MC_P_COUNT
} mc_param;

typedef struct mc_engine mc_engine;

// Returns NULL and fills err on failure.
mc_engine *mc_create(const uint8_t *main_os, size_t len, char *err, size_t errlen);
void mc_destroy(mc_engine *e);

void mc_set(mc_engine *e, int track, mc_param p, double value);
void mc_machine_defaults(mc_engine *e, int track, mc_machine m);

// Renders one block of all 6 voices. out[t][i] is the raw int32 voice output.
// Returns 0 on success, nonzero if the emulated code faulted.
int mc_block(mc_engine *e, uint32_t trig_mask, uint32_t release_mask, int32_t out[MC_TRACKS][MC_FRAMES]);
