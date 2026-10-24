#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int snesrecomp_rom_is_readable(const char *path);
int snesrecomp_rom_verify_crc32(const char *path, uint32_t expected_crc);
int snesrecomp_rom_verify_sha256(const char *path,
                                 const uint8_t *expected_sha256);
int snesrecomp_rom_match_sha256(const char *path,
                                const uint8_t (*hashes)[32],
                                size_t n_hashes);

/*
 * Quietly: 1 when the file's headerless payload has this SHA-256 (no dialog,
 * no log -- a folder scan probes files that are not ROMs at all).
 */
int snesrecomp_rom_sha256_equals(const char *path, const uint8_t expected[32]);

/*
 * Look through the player's game folder (snesrecomp_rom_search_dirs) for a
 * cartridge image (.sfc/.smc/.swc/.fig, any case, at most 16 MiB) whose
 * SHA-256 is `expected` -- a ROM dropped in the folder under any file name.
 * Returns 1 with its path in `out`.
 */
int snesrecomp_rom_locate_sha256(const uint8_t expected[32], char *out, size_t cap);
/* Same, from a 64-digit hex digest; 0 for a malformed digest. */
int snesrecomp_rom_locate_sha256_hex(const char *expected_hex, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
