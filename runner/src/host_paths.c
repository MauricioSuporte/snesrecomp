/*
 * host_paths.c — executable-relative and current-directory path policy.
 *
 * Kept separate from launcher.c because config, saves, mods, and command-line
 * paths all need this policy even when a game supplies its own ROM launcher.
 */
#include "host_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>
#  define snesrecomp_chdir _chdir
#  define snesrecomp_getcwd _getcwd
#else
#  include <unistd.h>
#  define snesrecomp_chdir chdir
#  define snesrecomp_getcwd getcwd
#endif
#ifdef __APPLE__
#  include <mach-o/dyld.h>
#endif

/* Full path of the running executable. Returns 1 on success. Platforms
 * without a query mechanism (e.g. Switch homebrew) return 0 and callers
 * fall back to cwd-relative behavior. */
static int get_exe_path(char *out, size_t max_len) {
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)max_len);
    return (n > 0 && n < max_len) ? 1 : 0;
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)max_len;
    return _NSGetExecutablePath(out, &size) == 0 ? 1 : 0;
#elif defined(__linux__)
    /* Inside an AppImage, /proc/self/exe resolves inside the read-only mount.
     * $APPIMAGE instead identifies the user-visible file beside config.ini,
     * rom.cfg, and saves. */
    const char *appimg = getenv("APPIMAGE");
    if (appimg && appimg[0]) {
        size_t len = strlen(appimg);
        if (len >= max_len) return 0;
        memcpy(out, appimg, len + 1);
        return 1;
    }
    ssize_t n = readlink("/proc/self/exe", out, max_len - 1);
    if (n <= 0) return 0;
    out[n] = '\0';
    return 1;
#else
    (void)out;
    (void)max_len;
    return 0;
#endif
}

/* Directory containing the executable, with trailing separator.
 * Falls back to "./" when the executable path cannot be determined. */
static void get_exe_dir(char *out, size_t max_len) {
    char exe_path[1024];
    if (get_exe_path(exe_path, sizeof(exe_path))) {
        char *last_sep = NULL;
        for (char *p = exe_path; *p; p++)
            if (*p == '/' || *p == '\\') last_sep = p;
        if (last_sep) {
            *(last_sep + 1) = '\0';
            snprintf(out, max_len, "%s", exe_path);
            return;
        }
    }
    snprintf(out, max_len, "./");
}

int snesrecomp_abspath(const char *path, char *out, size_t max_len) {
    if (!path || !*path || !out || max_len == 0) return 0;
#ifdef _WIN32
    char tmp[1024];
    if (!_fullpath(tmp, path, sizeof(tmp))) return 0;
    if (strlen(tmp) >= max_len) return 0;
    strcpy(out, tmp);
    return 1;
#else
    if (path[0] == '/') {
        if (strlen(path) >= max_len) return 0;
        strcpy(out, path);
        return 1;
    }
    char cwd[1024];
    if (!snesrecomp_getcwd(cwd, sizeof(cwd))) return 0;
    if (snprintf(out, max_len, "%s/%s", cwd, path) >= (int)max_len) return 0;
    return 1;
#endif
}

int snesrecomp_exe_path(char *out, size_t max_len) {
    if (!out || max_len == 0) return 0;
    out[0] = '\0';
    return get_exe_path(out, max_len);
}

int snesrecomp_exe_basename(char *out, size_t max_len) {
    char exe_path[1024];
    if (!out || max_len == 0) return 0;
    out[0] = '\0';
    if (!get_exe_path(exe_path, sizeof(exe_path))) return 0;

    const char *base = exe_path;
    for (const char *p = exe_path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    if (!*base) return 0;

    size_t len = strlen(base);
    /* Strip a trailing ".exe" so the same name serves the CMake target on
     * every platform. Case-insensitive: Windows does not promise the case. */
    if (len > 4) {
        const char *ext = base + len - 4;
        if ((ext[0] == '.') &&
            (ext[1] == 'e' || ext[1] == 'E') &&
            (ext[2] == 'x' || ext[2] == 'X') &&
            (ext[3] == 'e' || ext[3] == 'E'))
            len -= 4;
    }
    if (len >= max_len) return 0;
    memcpy(out, base, len);
    out[len] = '\0';
    return 1;
}

int snesrecomp_exe_dir_path(const char *leaf, char *out, size_t max_len) {
    if (!leaf || !out || max_len == 0) return 0;
    char dir[1024];
    get_exe_dir(dir, sizeof(dir));
    if (dir[0] == '.' &&
        (dir[1] == '/' || dir[1] == '\\' || dir[1] == '\0'))
        return 0;
    if (snprintf(out, max_len, "%s%s", dir, leaf) >= (int)max_len) return 0;
    return 1;
}

/* Probe by creating a file: access(W_OK) is unreliable on Windows and inside
 * sandboxed mounts. */
static int dir_is_writable(const char *dir) {
    char probe[1024];
    if (snprintf(probe, sizeof(probe), "%s.snesrecomp_write_probe",
                 dir) >= (int)sizeof(probe))
        return 0;
    FILE *f = fopen(probe, "wb");
    if (!f) return 0;
    fclose(f);
    remove(probe);
    return 1;
}

int snesrecomp_anchor_to_exe_dir(void) {
    char dir[1024];
    get_exe_dir(dir, sizeof(dir));
    if (dir[0] == '.' &&
        (dir[1] == '/' || dir[1] == '\\' || dir[1] == '\0'))
        return 0;
    if (!dir_is_writable(dir)) {
        fprintf(stderr,
                "[Launcher] Executable directory '%s' is not writable; "
                "config and saves stay in the current directory.\n", dir);
        return 0;
    }
    if (snesrecomp_chdir(dir) != 0) {
        fprintf(stderr, "[Launcher] Could not change directory to '%s'.\n", dir);
        return 0;
    }
    fprintf(stderr, "[Launcher] Config/saves anchored to '%s'.\n", dir);
    return 1;
}

/* ── Portable installs ────────────────────────────────────────────────────
 *
 * Every path the host writes for later (rom.cfg, the mods' external ROMs)
 * used to be absolute, so a game folder copied to a friend's PC opened on
 * "select your ROM" and the setup wizard even with both ROMs sitting in the
 * folder. Paths inside the game root are now stored relative to it, and a
 * stored path that no longer opens is looked for by name in the folder. */

static int path_readable(const char *path) {
    FILE *f;
    if (!path || !path[0]) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int path_is_absolute(const char *p) {
    if (!p || !p[0]) return 0;
    if (p[0] == '/' || p[0] == '\\') return 1;
#ifdef _WIN32
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
        p[1] == ':')
        return 1;
#endif
    return 0;
}

/* Strip trailing separators, keeping a bare root ("/" or "C:\"). */
static void strip_trailing_sep(char *s) {
    size_t n = strlen(s);
    while (n > 1 && (s[n - 1] == '/' || s[n - 1] == '\\')) {
#ifdef _WIN32
        if (n == 3 && s[1] == ':') break;
#endif
        s[--n] = '\0';
    }
}

/* Lexically normalise: '/' separators, no "." segments, ".." folded. */
static void normalise_path(const char *in, char *out, size_t cap) {
    char tmp[1024];
    char *segs[128];
    int n = 0;
    size_t prefix = 0;
    size_t o;
    snprintf(tmp, sizeof(tmp), "%s", in ? in : "");
    for (char *p = tmp; *p; ++p) if (*p == '\\') *p = '/';
    out[0] = '\0';
#ifdef _WIN32
    if (tmp[0] && tmp[1] == ':') prefix = 2;
#endif
    int absolute = tmp[prefix] == '/';
    int unc = !prefix && tmp[0] == '/' && tmp[1] == '/';
    char *cur = tmp + prefix + (absolute ? 1 : 0);
    char *tok = cur;
    for (;;) {
        char *slash = strchr(tok, '/');
        if (slash) *slash = '\0';
        if (!tok[0] || strcmp(tok, ".") == 0) {
            /* skip */
        } else if (strcmp(tok, "..") == 0) {
            if (n > 0 && strcmp(segs[n - 1], "..") != 0) --n;
            else if (!absolute && n < 128) segs[n++] = tok;
        } else if (n < 128) {
            segs[n++] = tok;
        }
        if (!slash) break;
        tok = slash + 1;
    }
    o = 0;
    if (prefix && o + 2 < cap) { out[o++] = tmp[0]; out[o++] = tmp[1]; }
    if (absolute && o + 1 < cap) out[o++] = '/';
    if (unc && o + 1 < cap) out[o++] = '/';   /* //server/share */
    for (int i = 0; i < n; ++i) {
        size_t len = strlen(segs[i]);
        if (i && o + 1 < cap) out[o++] = '/';
        if (o + len >= cap) break;
        memcpy(out + o, segs[i], len);
        o += len;
    }
    out[o < cap ? o : cap - 1] = '\0';
    if (!out[0] && cap > 1) { out[0] = '.'; out[1] = '\0'; }
}

static int dir_has_file(const char *dir, const char *leaf) {
    char p[1100];
    if (snprintf(p, sizeof(p), "%s/%s", dir, leaf) >= (int)sizeof(p)) return 0;
    return path_readable(p);
}

static int dirname_of(const char *path, char *out, size_t cap) {
    const char *cut = NULL;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') cut = p;
    if (!cut) return 0;
    size_t n = (size_t)(cut - path);
    if (n == 0) n = 1;  /* "/" */
#ifdef _WIN32
    if (n == 2 && path[1] == ':') n = 3;  /* "C:\" */
#endif
    if (n >= cap) return 0;
    memcpy(out, path, n);
    out[n] = '\0';
    return 1;
}

int snesrecomp_game_root_dir(char *out, size_t max_len) {
    char dir[1024], cur[1024];
    if (!out || max_len == 0) return 0;
    out[0] = '\0';
    get_exe_dir(dir, sizeof(dir));
    if (dir[0] == '.' && (dir[1] == '/' || dir[1] == '\\' || dir[1] == '\0')) {
        if (!snesrecomp_getcwd(dir, sizeof(dir))) return 0;
    }
    strip_trailing_sep(dir);
    snprintf(cur, sizeof(cur), "%s", dir);
    for (int up = 0; up <= 3; ++up) {
        if (dir_has_file(cur, "rom_identity.txt") ||
            dir_has_file(cur, "snesrecomp/snesrecomp_cli.py")) {
            snprintf(out, max_len, "%s", cur);
            return 1;
        }
        char parent[1024];
        if (!dirname_of(cur, parent, sizeof(parent)) || strcmp(parent, cur) == 0)
            break;
        snprintf(cur, sizeof(cur), "%s", parent);
    }
    snprintf(out, max_len, "%s", dir);
    return 1;
}

static int prefix_equal(const char *a, const char *b, size_t n) {
#ifdef _WIN32
    for (size_t i = 0; i < n; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
#else
    return strncmp(a, b, n) == 0;
#endif
}

int snesrecomp_portable_path(const char *path, char *out, size_t max_len) {
    char abs[1024], norm[1024], root_raw[1024], root[1024];
    if (!path || !path[0] || !out || max_len == 0) return 0;
    if (!snesrecomp_abspath(path, abs, sizeof(abs)))
        snprintf(abs, sizeof(abs), "%s", path);
    normalise_path(abs, norm, sizeof(norm));
    if (snesrecomp_game_root_dir(root_raw, sizeof(root_raw))) {
        normalise_path(root_raw, root, sizeof(root));
        size_t rn = strlen(root);
        if (rn > 0 && root[rn - 1] == '/') --rn;   /* filesystem root */
        if (rn > 0 && strlen(norm) > rn + 1 && norm[rn] == '/' &&
            prefix_equal(norm, root, rn)) {
            if (snprintf(out, max_len, "%s", norm + rn + 1) >= (int)max_len)
                return 0;
            return 1;
        }
    }
    if (snprintf(out, max_len, "%s", abs) >= (int)max_len) return 0;
    return 1;
}

static void add_dir(char dirs[][SNESRECOMP_SEARCH_PATH_LEN], int *n, int max,
                    const char *dir, const char *leaf) {
    char raw[SNESRECOMP_SEARCH_PATH_LEN + 16], norm[SNESRECOMP_SEARCH_PATH_LEN];
    if (*n >= max || !dir || !dir[0]) return;
    if (leaf) snprintf(raw, sizeof(raw), "%s/%s", dir, leaf);
    else snprintf(raw, sizeof(raw), "%s", dir);
    normalise_path(raw, norm, sizeof(norm));
    for (int i = 0; i < *n; ++i) {
        size_t a = strlen(dirs[i]);
        if (a == strlen(norm) && prefix_equal(dirs[i], norm, a)) return;
    }
    snprintf(dirs[(*n)++], SNESRECOMP_SEARCH_PATH_LEN, "%s", norm);
}

int snesrecomp_rom_search_dirs(char dirs[][SNESRECOMP_SEARCH_PATH_LEN], int max) {
    char root[1024], exe[1024], cwd[1024];
    int n = 0;
    if (!dirs || max <= 0) return 0;
    if (snesrecomp_game_root_dir(root, sizeof(root))) {
        add_dir(dirs, &n, max, root, NULL);
        add_dir(dirs, &n, max, root, "roms");
    }
    get_exe_dir(exe, sizeof(exe));
    if (!(exe[0] == '.' && (exe[1] == '/' || exe[1] == '\\' || exe[1] == '\0'))) {
        strip_trailing_sep(exe);
        add_dir(dirs, &n, max, exe, NULL);
        add_dir(dirs, &n, max, exe, "roms");
    }
    if (snesrecomp_getcwd(cwd, sizeof(cwd))) {
        add_dir(dirs, &n, max, cwd, NULL);
        add_dir(dirs, &n, max, cwd, "roms");
    }
    return n;
}

int snesrecomp_resolve_stored_path(const char *stored, char *out, size_t max_len) {
    char root[1024], cand[1100];
    const char *base;
    if (!out || max_len == 0) return 0;
    out[0] = '\0';
    if (!stored || !stored[0]) return 0;
    if (!snesrecomp_game_root_dir(root, sizeof(root))) root[0] = '\0';

    if (path_is_absolute(stored)) {
        if (path_readable(stored)) {
            snprintf(out, max_len, "%s", stored);
            return 1;
        }
    } else {
        if (root[0]) {
            snprintf(cand, sizeof(cand), "%s/%s", root, stored);
            if (path_readable(cand)) { normalise_path(cand, out, max_len); return 1; }
        }
        if (snesrecomp_exe_dir_path(stored, cand, sizeof(cand)) && path_readable(cand)) {
            normalise_path(cand, out, max_len);
            return 1;
        }
        if (path_readable(stored) && snesrecomp_abspath(stored, cand, sizeof(cand))) {
            normalise_path(cand, out, max_len);
            return 1;
        }
    }

    /* Not where it was written: look for the same file name in the folder. */
    base = stored;
    for (const char *p = stored; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    if (base[0]) {
        char dirs[SNESRECOMP_SEARCH_DIR_MAX][SNESRECOMP_SEARCH_PATH_LEN];
        int n = snesrecomp_rom_search_dirs(dirs, SNESRECOMP_SEARCH_DIR_MAX);
        for (int i = 0; i < n; ++i) {
            if (snprintf(cand, sizeof(cand), "%s/%s", dirs[i], base) >= (int)sizeof(cand))
                continue;
            if (path_readable(cand)) {
                snprintf(out, max_len, "%s", cand);
                return 1;
            }
        }
    }
    if (path_is_absolute(stored) || !root[0]) snprintf(out, max_len, "%s", stored);
    else {
        snprintf(cand, sizeof(cand), "%s/%s", root, stored);
        normalise_path(cand, out, max_len);
    }
    return 0;
}
