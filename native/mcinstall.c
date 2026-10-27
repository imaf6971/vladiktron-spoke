// Installs the firmware the virtual board runs from your own OS update file, the same way the app does.
//   ./mcinstall <model-cycles_OS1.13.syx> <dir>
// Writes <dir>/main_os.bin and <dir>/flash.factory.bin, and <dir>/flash.bin if there is none yet.
#include <stdio.h>

#include "mcfirmware.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <model-cycles_OS1.13.syx> <dir>\n", argv[0]);
        return 2;
    }
    char err[256];
    if (mcf_install(argv[1], argv[2], err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    printf("Model:Cycles OS 1.13 installed in %s\n", argv[2]);
    return 0;
}
