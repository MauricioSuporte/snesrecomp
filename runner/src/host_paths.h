#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Resolve `path` against the current working directory into an absolute path.
 * This also works for paths that do not exist yet. Use it for relative
 * command-line paths before calling snesrecomp_anchor_to_exe_dir().
 * Returns 1 on success.
 */
int snesrecomp_abspath(const char *path, char *out, size_t max_len);

/*
 * Build an absolute path for `leaf` inside the executable's directory,
 * independent of the current working directory. Returns 0 when the executable
 * directory cannot be determined.
 */
int snesrecomp_exe_dir_path(const char *leaf, char *out, size_t max_len);

/*
 * Full path of the running executable -- inside an AppImage, the .AppImage
 * file itself, which is what a relaunch must start. Returns 0 when the
 * platform has no way to ask.
 */
int snesrecomp_exe_path(char *out, size_t max_len);

/*
 * Basename of the running executable, without directory and without a
 * trailing ".exe". This is the name CMake built the target under, so it is
 * what a self-rebuild needs for both `cmake --build --target` and for finding
 * the freshly built binary to relaunch. Returns 0 when the executable path
 * cannot be determined (platforms without a query mechanism).
 */
int snesrecomp_exe_basename(char *out, size_t max_len);

/*
 * Change the working directory to the executable's writable directory so
 * relative config and save paths remain stable regardless of launch context.
 * Call this before opening files. If the directory cannot be determined or is
 * not writable, the current working directory remains unchanged. Returns 1
 * when the directory was changed and 0 when it was left alone.
 */
int snesrecomp_anchor_to_exe_dir(void);


/*
 * Portable installs. A player copies the whole game folder to another
 * machine (or another drive), so nothing a config file stores may depend on
 * where that folder used to be.
 *
 * The GAME ROOT is the folder the player ships: the nearest directory, from
 * the executable's own up to three levels above it, that holds the project
 * tree (rom_identity.txt or snesrecomp/snesrecomp_cli.py) -- so a game rebuilt
 * into <root>/build/ and the setup host at <root>/ share one root. Without the
 * tree it is the executable's directory. No trailing separator.
 */
int snesrecomp_game_root_dir(char *out, size_t max_len);

/*
 * The form of `path` to WRITE into a config file (rom.cfg, mod state, ...):
 * relative to the game root, with '/' separators, when the file lies inside
 * it; the absolute path otherwise (a ROM kept in Downloads stays where it is).
 * Returns 1 on success.
 */
int snesrecomp_portable_path(const char *path, char *out, size_t max_len);

/*
 * The path to OPEN for a value read back from a config file. An absolute path
 * that opens is used as is; a relative one is tried against the game root,
 * the executable's directory and the working directory. When none opens --
 * an absolute path written on another machine, say -- the file's own name is
 * looked for in each search directory (see below). Returns 1 with a readable
 * file in `out`, 0 when nothing opened (then `out` holds the stored value made
 * absolute against the game root, for messages).
 */
int snesrecomp_resolve_stored_path(const char *stored, char *out, size_t max_len);

/*
 * Directories a player is expected to drop ROMs into, most specific first,
 * without duplicates: the game root and its roms/ folder, the executable's
 * directory and its roms/ folder, the working directory and its roms/ folder.
 * Returns how many were written (at most `max`).
 */
enum { SNESRECOMP_SEARCH_DIR_MAX = 8, SNESRECOMP_SEARCH_PATH_LEN = 1024 };
int snesrecomp_rom_search_dirs(char dirs[][SNESRECOMP_SEARCH_PATH_LEN], int max);

#ifdef __cplusplus
}
#endif
