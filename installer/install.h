#pragma once
#include "setup.h"

#include "game.h"
#define UNINSTALL_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" GAME_ID "_PCNative"

typedef struct {
    char cd[MAX_PATH * 2], dir[MAX_PATH * 2];
    char patch[MAX_PATH * 2];      /* a second source some games need (official patch), see game_patch_label */
    int desktop, startmenu;
} InstallOpts;

int do_install(const InstallOpts *o, void (*cb)(u64 done, u64 total, void *user), void *user);
int do_uninstall(const char *dir);
int prebuilt_available(void);
/* installer/game_install.c */
int game_is_cd(CdSrc *c);
int game_install_files(const char *cd, const char *hd);   /* what the DOS installer put on C: */
const char *game_icon_lbm(void);                          /* LBM on the CD for the exe icon, or NULL */
/* optional (weak) game hooks */
const char *game_patch_label(void) __attribute__((weak));               /* show a "patch" row: its label */
const char *game_patch_hint(void) __attribute__((weak));                /* one line about what it is */
int game_patch_find(const char *dir, char *out, size_t n) __attribute__((weak));   /* a patch next to setup / the CD */
int game_install_patch(const char *patch, const char *hd) __attribute__((weak));
u64 game_audio_bytes(CdSrc *c) __attribute__((weak));                  /* CD music: bytes it will write */
int game_install_audio(CdSrc *c, const char *game, Progress *pr) __attribute__((weak));
int inject_icon_lbm(const char *exe, const u8 *lbm, size_t n);
void shortcut_paths(char *startmenu, char *desktop);
