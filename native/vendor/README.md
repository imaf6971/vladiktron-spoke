# Vendored source

## `unicorn/`: Unicorn 2.1.4, modified

[Unicorn](https://github.com/unicorn-engine/unicorn) at tag `2.1.4` (commit `8028ec436f2d9376525352dd38ed9ed6b9f6be10`),
GPL-2.0 (`unicorn/COPYING`; its GLib parts LGPL-2.0, `unicorn/COPYING_GLIB`). It is kept here so the app builds
without fetching anything, and so this repository is the complete source the GPL asks for.

Left out, since the build does not use them: `bindings/`, `docs/`, `tests/`, `samples/`, `msvc/`.

**Modified** by this project (2026-10, Миронов Владислав Андреевич) for the ColdFire CPU of the Model:Cycles:
the EMAC unit, QEMU's own exceptions, IRQ lines, safe stops, the m68k SR read, and speed (stores to RAM pages without
code take the fast path, the JIT stays executable for a whole run). `../patch_unicorn.py` makes these changes to a
stock 2.1.4 and explains each one; `unicorn-2.1.4.patch` is the resulting diff. The files here already have them
applied (running the script on them again changes nothing).

To move to another Unicorn release: check it out, run `python3 ../patch_unicorn.py <checkout>`, fix what no longer
applies, copy it over `unicorn/` (without the folders above) and regenerate the patch with `git diff`.
