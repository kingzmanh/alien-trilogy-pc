/* What the installer does that is particular to Alien Trilogy.
 *
 * The CD is copied whole to <install>\game\cd (the game's D: drive).
 * <install>\game\hd is the game's C: drive, as the DOS INSTALL left it:
 *   TRILOGY.EXE - not on the CD as a file: the DOS installer unpacks it from
 *     T16P.SHR (16-bit colour, Pentium).  The port is built from the official
 *     European 2.0b patch (ATUPDEU1, from the "Fixes" download): its T16P.SHR
 *     is unpacked here and checked (size + CRC) - the recompiled code belongs
 *     to exactly that program.
 *   TRILOGY.CFG - the default settings (ET.SHR, English)
 *   PATH.TXT    - the CD drive ("D:\")
 *   DUKDOS.EXE  - the movie player the game starts
 * The music is Red Book audio (tracks 2-17): ripped from the image into
 * <install>\game\cdaudio\trackNN.wav with the disc's table of contents in
 * toc.txt (src/rt/cdaudio.c plays them). */
#include "setup.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "install.h"
#include "unzip.h"

u8 *shr_unpack(const u8 *shr, size_t n, char *name, size_t namesz, size_t *outn);

#define EXE_SIZE 786347u        /* TRILOGY.EXE of ATUPDEU1 T16P.SHR (EU 2.0b, 16-bit, Pentium) */
#define EXE_CRC  0x1bd6d2c4u

int game_is_cd(CdSrc *c)
{
    static const char *files[] = GAME_CD_FILES;
    if (!c) return 0;
    for (int i = 0; files[i]; i++)
        if (!cd_exists(c, files[i])) return 0;
    return 1;
}

/* unpack one .SHR from the copied CD into hd under its own name or `as` */
static int unpack_shr(const char *shr, const char *hd, const char *as)
{
    size_t n, on;
    u8 *d = read_file(shr, &n);
    if (!d) { setup_log("ERROR: cannot read %s", shr); return 0; }
    char name[64];
    u8 *o = shr_unpack(d, n, name, sizeof name, &on);
    free(d);
    if (!o) { setup_log("ERROR: %s is damaged", shr); return 0; }
    char p[MAX_PATH * 2];
    snprintf(p, sizeof p, "%s\\%s", hd, as ? as : name);
    int ok = write_file(p, o, on);
    free(o);
    return ok;
}

int game_install_files(const char *cd, const char *hd)
{
    char s[MAX_PATH * 2], d[MAX_PATH * 2];
    snprintf(s, sizeof s, "%s\\ET.SHR", cd);
    if (!unpack_shr(s, hd, "TRILOGY.CFG")) return 0;
    {   /* keys: WASD layout by default (TRILOGY.CFG offset 3, 14 scancodes; the
           Input menu switches back to the keypad layout) - same table as src/game/game.c */
        static const u8 wasd[14] = {0x11, 0x1f, 0x4b, 0x4d, 0x1e, 0x20, 0x38, 0x1d, 0x10, 0x39, 0x12, 0x13, 0x21, 0x0e};
        size_t n;
        snprintf(d, sizeof d, "%s\\TRILOGY.CFG", hd);
        u8 *cfg = read_file(d, &n);
        if (cfg && n >= 17) { memcpy(cfg + 3, wasd, 14); write_file(d, cfg, n); }
        free(cfg);
    }
    snprintf(s, sizeof s, "%s\\DUKDOS.EXE", cd);
    snprintf(d, sizeof d, "%s\\DUKDOS.EXE", hd);
    if (!copy_file(s, d, NULL)) { setup_log("ERROR: cannot write %s", d); return 0; }
    SetFileAttributesA(d, FILE_ATTRIBUTE_NORMAL);
    snprintf(d, sizeof d, "%s\\PATH.TXT", hd);
    return write_file(d, "D:\\\r\n", 5);
}

/* ---- the official patch ---- */
const char *game_patch_label(void) { return "Patch:"; }
const char *game_patch_hint(void)
{
    return "Official EU patch 2.0b: the \"Fixes\" zip, its ATUPDEU1 folder or T16P.SHR";
}

static int ieq_end(const char *s, const char *end)
{
    size_t a = strlen(s), b = strlen(end);
    return a >= b && !_stricmp(s + a - b, end);
}

static int want_eu(const char *path, void *ud)
{
    (void)ud;
    char low[260];
    size_t i = 0;
    for (; path[i] && i < sizeof low - 1; i++) low[i] = (char)tolower((unsigned char)path[i]);
    low[i] = 0;
    return strstr(low, "atupdeu1") && ieq_end(low, "t16p.shr");
}

static int want_any(const char *path, void *ud) { (void)ud; return ieq_end(path, "t16p.shr"); }

int game_patch_find(const char *dir, char *out, size_t n)
{
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        char low[MAX_PATH];
        snprintf(low, sizeof low, "%s", fd.cFileName);
        for (char *q = low; *q; q++) *q = (char)tolower((unsigned char)*q);
        int dirent = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if ((!dirent && ieq_end(low, ".zip") && (strstr(low, "fix") || strstr(low, "atupd") || strstr(low, "patch"))) ||
            (dirent && !strcmp(low, "atupdeu1"))) {
            snprintf(out, n, "%s\\%s", dir, fd.cFileName);
            found = 1;
        }
    } while (!found && FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

int game_install_patch(const char *patch, const char *hd)
{
    char shr[MAX_PATH * 2] = "", tmp[MAX_PATH * 2] = "";
    if (!patch || !patch[0]) {
        setup_log("ERROR: the game needs the official EU patch 2.0b (ATUPDEU1) - choose its zip or folder as the patch");
        return 0;
    }
    if (is_dir(patch)) {
        static const char *tries[] = {"\\T16P.SHR", "\\ATUPDEU1\\T16P.SHR", "\\Alien_Trilogy_Fixes\\atupdeu1\\T16P.SHR"};
        for (int i = 0; i < 3 && !shr[0]; i++) {
            char p[MAX_PATH * 2];
            snprintf(p, sizeof p, "%s%s", patch, tries[i]);
            if (path_exists(p)) snprintf(shr, sizeof shr, "%s", p);
        }
    } else if (ieq_end(patch, ".zip")) {
        snprintf(tmp, sizeof tmp, "%s\\patch.tmp", hd);
        make_dirs(tmp);
        if (zip_extract_path(patch, tmp, want_eu, NULL, NULL) <= 0 && zip_extract_path(patch, tmp, want_any, NULL, NULL) <= 0) {
            delete_tree(tmp);
            setup_log("ERROR: %s holds no T16P.SHR - it is not the Alien Trilogy patch", patch);
            return 0;
        }
        snprintf(shr, sizeof shr, "%s\\T16P.SHR", tmp);
        if (!path_exists(shr)) snprintf(shr, sizeof shr, "%s\\t16p.shr", tmp);
    } else snprintf(shr, sizeof shr, "%s", patch);
    if (!shr[0] || !path_exists(shr)) {
        setup_log("ERROR: no T16P.SHR in %s", patch);
        if (tmp[0]) delete_tree(tmp);
        return 0;
    }
    size_t n, on;
    u8 *d = read_file(shr, &n);
    if (tmp[0]) delete_tree(tmp);
    if (!d) { setup_log("ERROR: cannot read %s", shr); return 0; }
    char name[64];
    u8 *exe = shr_unpack(d, n, name, sizeof name, &on);
    free(d);
    if (!exe) { setup_log("ERROR: %s is damaged", shr); return 0; }
    if (on != EXE_SIZE || crc32_buf(exe, on) != EXE_CRC) {
        free(exe);
        setup_log("ERROR: this patch's TRILOGY.EXE is not the European 2.0b one (ATUPDEU1, 16-bit colour) this port is built for");
        return 0;
    }
    char p[MAX_PATH * 2];
    snprintf(p, sizeof p, "%s\\TRILOGY.EXE", hd);
    int ok = write_file(p, exe, on);
    free(exe);
    setup_log("TRILOGY.EXE (EU 2.0b) unpacked from the patch");
    return ok;
}

/* ---- the CD's audio tracks -> game\cdaudio ---- */
u64 game_audio_bytes(CdSrc *c)
{
    CdTrack t[100];
    int n = cd_tracks(c, t, 100);
    u64 b = 0;
    for (int i = 0; i < n; i++) if (t[i].audio) b += (u64)t[i].nsec * 2352 + 44;
    return b;
}

int game_install_audio(CdSrc *c, const char *game, Progress *pr)
{
    CdTrack t[100];
    int n = cd_tracks(c, t, 100);
    char dir[MAX_PATH * 2], p[MAX_PATH * 3];
    snprintf(dir, sizeof dir, "%s\\cdaudio", game);
    int naudio = 0;
    for (int i = 0; i < n; i++) naudio += t[i].audio;
    if (!naudio) {
        setup_log("No CD audio tracks in this source - the music needs the disc or an image with its tracks (.ccd/.img, .cue/.bin, .nrg).");
        return 1;
    }
    make_dirs(dir);
    char toc[4096];
    int tl = snprintf(toc, sizeof toc, "# track start_lba sectors (track 1 = data)\r\n");
    u32 leadout = 0;
    static u8 buf[75 * 2352];
    for (int i = 0; i < n; i++) {
        tl += snprintf(toc + tl, sizeof toc - tl, "%d %u %u\r\n", t[i].no, t[i].lba, t[i].nsec);
        if (t[i].lba + t[i].nsec > leadout) leadout = t[i].lba + t[i].nsec;
        if (!t[i].audio) continue;
        snprintf(p, sizeof p, "%s\\track%02d.wav", dir, t[i].no);
        HANDLE out = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (out == INVALID_HANDLE_VALUE) { setup_log("ERROR: cannot write %s", p); return 0; }
        u32 data = t[i].nsec * 2352;
        u8 hdr[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
                      1, 0, 2, 0, 0x44, 0xac, 0, 0, 0x10, 0xb1, 2, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a'};
        u32 riff = data + 36;
        memcpy(hdr + 4, &riff, 4);
        memcpy(hdr + 40, &data, 4);
        DWORD w;
        WriteFile(out, hdr, 44, &w, NULL);
        for (u32 s = 0; s < t[i].nsec; s += 75) {
            u32 k = t[i].nsec - s < 75 ? t[i].nsec - s : 75;
            if (!cd_read_raw(c, i, s, k, buf)) { CloseHandle(out); setup_log("ERROR: reading audio track %d failed", t[i].no); return 0; }
            if (!WriteFile(out, buf, k * 2352, &w, NULL) || w != k * 2352) { CloseHandle(out); setup_log("ERROR: cannot write %s", p); return 0; }
            if (pr) progress_add(pr, k * 2352);
        }
        CloseHandle(out);
    }
    tl += snprintf(toc + tl, sizeof toc - tl, "leadout %u\r\n", leadout);
    snprintf(p, sizeof p, "%s\\toc.txt", dir);
    setup_log("CD music: %d tracks", naudio);
    return write_file(p, toc, (size_t)tl);
}

const char *game_icon_lbm(void) { return GAME_ICON_LBM; }
