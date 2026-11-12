/* What is particular to this game.  The runtime (src/rt) calls these hooks.
 *
 * game_main is the launcher.  A game that is one DOS program just runs it;
 * a game with a DOS launcher (a menu program, a batch file, MCP.EXE in
 * Alien Odyssey) gets that launcher rewritten here: rt_run_program(name,
 * args) runs one program on fresh guest memory and returns its exit code.
 * See libs/examples/main_odyssey.c (mcp()) for a six-program chain driven
 * by exit codes, with save-slot and password arguments. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include "rt.h"

void game_main(int state)
{
    (void)state;   /* --state <hex>: free for the launcher (Alien Odyssey: which program/save to start with) */
    /* AT_ARGS: the game's own command line for tests (-nomovies, -nocd, ...) */
    const char *a = getenv(ENV("ARGS"));
    rt_run_program(GAME_FIRST_PROGRAM, a ? a : "");
}

/* a program is about to start on fresh memory: reset library state */
void game_program_start(const char *name)
{
    (void)name;
#if HAVE_AIL || HAVE_SOS
    sound_reset();
#endif
#if HAVE_HIRES3D
    { extern void hires3d_reset(void); hires3d_reset(); }
#endif
#if HAVE_HDTEXT
    hdtext_reset();
#endif
}

const char *game_controls_text(void)
{
    return "The game's own controls, as on DOS.\n\n"
           "PC: Alt+Enter switches fullscreen; in a window the menu bar has the video options.";
}

const char *game_about_text(void)
{
    return GAME_TITLE " - PC Native\n\n"
           "The DOS game's code converted to native Windows code, running on a PC\n"
           "runtime that replaces DOS, the extender, and the sound and video hardware.\n\n"
           "Fan preservation project - you need your own copy of the game.";
}

/* game items in the Input menu (ids from 2000), e.g. switching the game's
   own control setting in its config file - see the Alien Odyssey port */
/* ---- key layouts ----
   The game keeps one keyboard key per action at 8021b8 (14 scancodes, saved in
   TRILOGY.CFG at offset 3): forward, back, turn left, turn right, strafe left,
   strafe right, strafe modifier, fire, fire 2, use, next weapon, look up,
   look down, turn around.  Its own Options screen can redefine them. */
static const unsigned char keys_wasd[14] = {
    0x11, 0x1f, 0x4b, 0x4d,     /* W, S, Left, Right (arrows = keypad codes) */
    0x1e, 0x20, 0x38, 0x1d,     /* A, D, Alt, Ctrl */
    0x10, 0x39, 0x12,           /* Q fire 2, Space use, E next weapon */
    0x13, 0x21, 0x0e};          /* R look up, F look down, Backspace turn around */
static const unsigned char keys_dos[14] = {
    0x48, 0x50, 0x4b, 0x4d, 0x33, 0x34, 0x38, 0x1d, 0x4c, 0x39, 0x07, 0x47, 0x4f, 0x0e};
enum { ID_KEYS_WASD = 2010, ID_KEYS_DOS, ID_NET_JOIN = 2020, ID_NET_LAN };
extern int net_ask_address(void *parent);
extern void net_address(char *out, int n);
extern void settings_set_str(const char *sec, const char *key, const char *v);

static int keys_layout_now(void)
{
    if (!memcmp(MEM + 0x8021b8, keys_wasd, 14)) return 1;
    if (!memcmp(MEM + 0x8021b8, keys_dos, 14)) return 2;
    return 0;   /* the player's own keys */
}

static void keys_set(const unsigned char *k)
{
    memcpy(MEM + 0x8021b8, k, 14);   /* the game's live table */
    memcpy(MEM + 0x802039, k, 14);   /* its copy of the config file */
    char p[700];
    snprintf(p, sizeof p, "%s\\hd\\TRILOGY.CFG", rt_root);
    FILE *f = fopen(p, "r+b");
    if (f) { fseek(f, 3, SEEK_SET); fwrite(k, 1, 14, f); fclose(f); }
}

void game_menu_build(void *input_menu)
{
    HMENU m = (HMENU)input_menu;
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_KEYS_WASD, L"Keys: &WASD  (W/S move, A/D strafe, arrows turn, Q/E/R/F)");
    AppendMenuW(m, MF_STRING, ID_KEYS_DOS, L"Keys: &original  (keypad)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_NET_JOIN, L"Multiplayer: &join over the internet...");
    AppendMenuW(m, MF_STRING, ID_NET_LAN, L"Multiplayer: &LAN only");
}

/* ---- Video menu: the game's own options (sttng_pc-style [game] keys) ---- */
enum { ID_MOVIELINES = 2001, ID_SKIPINTRO, ID_WIDEVIEW, ID_SPEED = 2100 };
extern int settings_int(const char *sec, const char *key, int def, int lo, int hi);
extern void settings_set_int(const char *sec, const char *key, int v);

void game_video_menu_build(void *video_menu)
{
    HMENU m = (HMENU)video_menu;
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_MOVIELINES, L"Full-line &movies  (no black lines between rows)");
    AppendMenuW(m, MF_STRING, ID_SKIPINTRO, L"&Skip the logo/intro movies at start");
    AppendMenuW(m, MF_STRING, ID_WIDEVIEW, L"&Wide view  (true widescreen: the 3D view fills a wide window or screen)");
    HMENU sp = CreatePopupMenu();
    AppendMenuW(sp, MF_STRING, ID_SPEED + 75, L"75%");
    AppendMenuW(sp, MF_STRING, ID_SPEED + 100, L"100%  (the game's own rate, 34.5 steps/s - default)");
    AppendMenuW(sp, MF_STRING, ID_SPEED + 125, L"125%");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sp, L"Game &speed");
}

void game_menu_update(void *menu)
{
    HMENU m = (HMENU)menu;
    CheckMenuItem(m, ID_MOVIELINES, MF_BYCOMMAND | (settings_int("game", "MovieLines", 1, 0, 1) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, ID_SKIPINTRO, MF_BYCOMMAND | (settings_int("game", "SkipIntro", 0, 0, 1) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, ID_WIDEVIEW, MF_BYCOMMAND | (settings_int("game", "WideView", 1, 0, 1) ? MF_CHECKED : MF_UNCHECKED));
    int kl = keys_layout_now();
    CheckMenuItem(m, ID_KEYS_WASD, MF_BYCOMMAND | (kl == 1 ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, ID_KEYS_DOS, MF_BYCOMMAND | (kl == 2 ? MF_CHECKED : MF_UNCHECKED));
    {   /* the join address in the menu text */
        char a[200];
        wchar_t t[300];
        net_address(a, sizeof a);
        if (*a) swprintf(t, 300, L"Multiplayer: &join over the internet...  (now: %hs)", a);
        else swprintf(t, 300, L"Multiplayer: &join over the internet...");
        ModifyMenuW(m, ID_NET_JOIN, MF_BYCOMMAND | MF_STRING | (*a ? MF_CHECKED : MF_UNCHECKED), ID_NET_JOIN, t);
        CheckMenuItem(m, ID_NET_LAN, MF_BYCOMMAND | (*a ? MF_UNCHECKED : MF_CHECKED));
    }
    int sp = settings_int("game", "GameSpeed", 100, 25, 400);
    CheckMenuRadioItem(m, ID_SPEED + 75, ID_SPEED + 125, ID_SPEED + sp, MF_BYCOMMAND);
}

int game_menu_command(int id)
{
    if (id == ID_KEYS_WASD) { keys_set(keys_wasd); return 1; }
    if (id == ID_KEYS_DOS) { keys_set(keys_dos); return 1; }
    if (id == ID_NET_JOIN) { net_ask_address(GetActiveWindow()); return 1; }
    if (id == ID_NET_LAN) { extern void net_reload(void); settings_set_str("net", "Connect", ""); net_reload(); return 1; }
    if (id == ID_MOVIELINES) { settings_set_int("game", "MovieLines", !settings_int("game", "MovieLines", 1, 0, 1)); return 1; }
    if (id == ID_WIDEVIEW) { settings_set_int("game", "WideView", !settings_int("game", "WideView", 1, 0, 1)); return 1; }
    if (id == ID_SKIPINTRO) { settings_set_int("game", "SkipIntro", !settings_int("game", "SkipIntro", 0, 0, 1)); return 1; }
    if (id == ID_SPEED + 75 || id == ID_SPEED + 100 || id == ID_SPEED + 125) { settings_set_int("game", "GameSpeed", id - ID_SPEED); return 1; }
    return 0;
}

/* video.c: fill the empty rows the movie player leaves (only while it runs) */
int game_fill_movie_lines(void)
{
    extern const char *rt_module;
    return !_stricmp(rt_module, "DUKDOS") && settings_int("game", "MovieLines", 1, 0, 1);
}

/* main.c rt_exec_program asks before a nested program starts: the start-up
   logo/intro chain (DUKDOS 14) can be skipped (it cannot be skipped by key) */
int game_exec_allow(const char *name, const char *args)
{
    if (!_stricmp(name, "DUKDOS") && !strcmp(args, "14") && settings_int("game", "SkipIntro", 0, 0, 1)) return 0;
    return 1;
}

/* new ini: the port's best options on, the game's own behaviour otherwise */
const char *game_default_ini(void)
{
    return "\r\n[game]\r\n"
           "; MovieLines: 1 = movies without the black lines between rows (0 = as on DOS)\r\nMovieLines=1\r\n"
           "; SkipIntro: 1 = skip the logo/intro movies when the game starts\r\nSkipIntro=0\r\n"
           "; WideView: 1 = true widescreen: the 3D view is as wide as the window/screen (HUD stays in the middle), 0 = 4:3\r\nWideView=1\r\n"
           "; GameSpeed: percent of the game's own rate (100 = 34.5 logic steps per second)\r\nGameSpeed=100\r\n"
           "; Mouse options (also in the game's Options panel): Mouse 1 = the mouse moves the player,\r\n"
           "; MouseSpeed 1-5 (3 = normal, 1 = half, 5 = double), MouseLook 1 = mouse up/down looks up/down\r\n"
           "Mouse=1\r\nMouseSpeed=3\r\nMouseLook=1\r\n"
           "\r\n[net]\r\n"
           "; Multiplayer (main menu): Connect = the host's address for a game over the internet\r\n"
           "; (IP or name[:port]; empty = LAN, games are found by themselves). Port = UDP port (the host forwards it)\r\n"
           "; Tip: easiest over the internet - everyone installs Radmin VPN (free), joins the same network,\r\n"
           "; and the others set Connect to the host's Radmin IP (26.x.x.x): no router setup needed\r\n"
           "Connect=\r\nPort=17733\r\n";
}

/* MSCDEX 1505h: the volume label of the Alien Trilogy CD */
const char *game_cd_label(void) { return "ALIEN_TRILOGY_PC"; }
