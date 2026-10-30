"""Reference output for mcbench's `scenario` mode, from the Modded-Cycles Python engine (exact EMAC).

    python ref_scenario.py <Modded-Cycles>/tools <main_os.bin> <out.raw>

Keep the scenario identical to scenario_step() in mcbench.c.
"""
import pathlib
import sys

sys.path.insert(0, sys.argv[1] + '/emu')
sys.path.insert(0, sys.argv[1])
import numpy as np
import mcengine as E

eng = E.Engine(pathlib.Path(sys.argv[2]).read_bytes())
for t, m in enumerate(E.MACH):
    eng.machine_defaults(t, m)
color0 = [10, 0, 46, 38, 40, 3]
shape0 = [16, 127, 48, 100, 38, 43]
out = []
for b in range(360):
    trig = rel = 0
    if b == 0:
        trig = 0x3f
    if b == 120:
        for t in range(6):
            eng.set(t, pitch=54 + 5 * t, color=(color0[t] + 17) % 128, shape=(shape0[t] + 31) % 128,
                    decay=30 + 7 * t, note=60 + 2 * t)
        trig = 0x3f
    if b == 200:
        rel = 0x3f
    if b == 240:
        for t in range(6):
            eng.set(t, sweep=90, contour=10, punch=64, finetune=70)
        trig = 0x15
    out.append(eng.block(trig, rel).astype('<i4'))
    if b % 60 == 59:
        print(f'  {b + 1}/360 blocks', flush=True)
np.stack(out).tofile(sys.argv[3])
print('unmapped accesses:', eng.unmapped[:5])
