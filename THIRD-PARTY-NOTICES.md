# Third-party notices

Model Mac is licensed under the GNU General Public License, version 2 only (`LICENSE`). It includes or builds on
the following. Elektron's firmware is not part of it: the app installs it from the user's own OS update file.

| Component | Where | Licence |
|---|---|---|
| [Unicorn](https://www.unicorn-engine.org/) 2.1.4 (Nguyen Anh Quynh, Dang Hoang Vu and the Unicorn contributors), with QEMU code inside | Linked statically into the app and the command-line tools. Fetched by `native/build.sh` and changed by `native/patch_unicorn.py` | GPL-2.0 (`LICENSE`); its GLib parts LGPL-2.0 ([COPYING_GLIB](https://github.com/unicorn-engine/unicorn/blob/2.1.4/COPYING_GLIB)) |
| [Modded-Cycles](https://github.com/18nelli18/Modded-Cycles) (18nelli18 and contributors) | `native/mcengine.c` (a C port of its `mcengine.py`), the EMAC model in `native/patch_unicorn.py` (after its `emac.py`), OS 1.13 addresses in `native/mcboard.c`, and `native/mcfirmware.c` (after its `mtlib`) | MIT |
| [elektron-model-tweaks](https://github.com/drumkilla/elektron-model-tweaks) (drumkilla) | `mtlib`, the SysEx, container and aPLib-variant code `native/mcfirmware.c` is ported from | MIT |
| [elektron-firmware-tool](https://github.com/mischa85/elektron-firmware-tool) (Marcel Bierling) | The basis of `mtlib`'s container and codec code | MIT |

The source for this app, including the Unicorn patches, is at https://github.com/imaf6971/model-mac.

Model:Cycles and Elektron are trademarks of Elektron Music Machines MAV AB. This project is not affiliated with or
endorsed by Elektron. The names are used only to say which device's OS it runs.

## MIT licence texts

### Modded-Cycles

Copyright (c) 2026 18nelli18 and Modded-Cycles contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### elektron-model-tweaks (mtlib)

Copyright (c) 2026 drumkilla

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### elektron-firmware-tool

Copyright (c) 2026 Marcel Bierling

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
