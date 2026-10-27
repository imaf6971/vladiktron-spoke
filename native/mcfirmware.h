// Reads the user's own Model:Cycles OS update file (.syx) into what the virtual board runs: the decompressed MAIN
// OS (section 3) and a 2 MB SPI-flash image with the firmware container at 0x20000. Only Elektron's original OS
// 1.13 is accepted (version string and SHA-256 checked), since every address in mcboard.c is specific to it.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MCF_FLASH_SIZE 0x200000

typedef struct {
    uint8_t *main_os;          // malloc'd; load address 0x40000400
    size_t main_os_len;
    uint8_t *flash;            // malloc'd, MCF_FLASH_SIZE bytes
    char version[16];          // as the file names it, e.g. "1.13"
} mcf_firmware;

// Returns 0 and fills `out`, or -1 with a message for the user in `err`.
int mcf_decode(const uint8_t *syx, size_t len, mcf_firmware *out, char *err, size_t errlen);
void mcf_free(mcf_firmware *fw);

// Decodes `syx_path` and writes main_os.bin and flash.factory.bin into `dir`, plus flash.bin if there is none yet
// (it holds the OS's settings, so an existing one is kept). Each file is written whole, then renamed into place.
int mcf_install(const char *syx_path, const char *dir, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
