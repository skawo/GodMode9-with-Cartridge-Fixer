#include "fixerui.h"

#include <stdarg.h>

#include "ui.h"
#include "hid.h"
#include "timer.h"

#define UI_W             SCREEN_WIDTH_ALT
#define FIXERUI_REFRESH_MS 120

// layout
#define Y_TITLE          0
#define Y_PHASE          18
#define Y_SUB            30
#define Y_BAR            42
#define Y_COUNTS         58
#define Y_FLAGS          70
#define Y_LIMITS         82
#define Y_SEP1           94
#define Y_ELAPSED        102
#define Y_SEP2           114
#define Y_HELP1          126
#define Y_HELP2          138
#define Y_HELP3          150

static bool ui_active = false;
static FixerUiPhase ui_phase = FIXERUI_PHASE_IDLE;
static u32 ui_sub_cur = 0, ui_sub_total = 0;
static u32 ui_fixed = 0, ui_bad = 0;
static bool ui_autoskip = false, ui_log = false, ui_refresh_read = false;
static u32 ui_retry = FIXER_CFG_DEFAULT_RETRIES;
static u32 ui_stuck = FIXER_CFG_DEFAULT_STUCK;

static u64 ui_start = 0;
static u64 ui_last_hb = 0;
static u64 ui_last_draw = 0;
static u32 ui_spin = 0;
static u32 ui_tick_ms = 0;
static u32 ui_max_tick_ms = 0;
static u32 ui_bar_pct = 0xFFFFFFFFu;

// cached dynamic strings, to only redraw what changed
static char c_phase[64] = { 0 };
static char c_sub[64] = { 0 };
static char c_counts[64] = { 0 };
static char c_flags[64] = { 0 };
static char c_limits[64] = { 0 };
static char c_elapsed[64] = { 0 };
static char c_status[64] = { 0 };

static const char* phase_name(FixerUiPhase phase) {
    switch (phase) {
        case FIXERUI_PHASE_OPEN:       return "Opening";
        case FIXERUI_PHASE_HEADERS:    return "Reading headers";
        case FIXERUI_PHASE_EXT_HASH:   return "ExtHeader hash";
        case FIXERUI_PHASE_EXEFS_HASH: return "ExeFS hash";
        case FIXERUI_PHASE_ROMFS_HASH: return "ROMFS hash";
        case FIXERUI_PHASE_EXEFS:      return "ExeFS files";
        case FIXERUI_PHASE_ROMFS:      return "ROMFS level-3";
        case FIXERUI_PHASE_DONE:       return "Done";
        default:                       return "Idle";
    }
}

static void draw_static(void) {
    DrawRectangle(ALT_SCREEN, 0, Y_TITLE, UI_W, 13, COLOR_STD_FONT);
#ifdef FIXER_SIM
    DrawString(ALT_SCREEN, "CARTRIDGE FIXER (SIM)", 4, Y_TITLE + 2, COLOR_STD_BG, COLOR_STD_FONT);
#else
    DrawString(ALT_SCREEN, "CARTRIDGE FIXER", 4, Y_TITLE + 2, COLOR_STD_BG, COLOR_STD_FONT);
#endif

    DrawRectangle(ALT_SCREEN, 0, Y_SEP1, UI_W, 1, COLOR_DARKGREY);
    DrawRectangle(ALT_SCREEN, 0, Y_SEP2, UI_W, 1, COLOR_DARKGREY);

    DrawString(ALT_SCREEN, "B   abort fixing", 4, Y_HELP1, COLOR_LIGHTGREY, COLOR_STD_BG);
    DrawString(ALT_SCREEN, "Y   skip bad block (hold)", 4, Y_HELP2, COLOR_LIGHTGREY, COLOR_STD_BG);
    DrawString(ALT_SCREEN, "Config set in pre-flight", 4, Y_HELP3, COLOR_DARKGREY, COLOR_STD_BG);
}

// Redraw a line only when its text changed (saves the bus and avoids flicker)
static void draw_cached(int x, int y, u32 color, char* cache, const char* fmt, ...) {
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!strcmp(buf, cache)) return;
    snprintf(cache, 64, "%s", buf);
    DrawRectangle(ALT_SCREEN, 0, y, UI_W, 9, COLOR_STD_BG);
    DrawString(ALT_SCREEN, buf, x, y, color, COLOR_STD_BG);
}

static void draw_bar(void) {
    u32 pct = 0;
    if (ui_sub_total)
        pct = (ui_sub_cur >= ui_sub_total) ? 100 : (u32) (((u64) ui_sub_cur * 100) / ui_sub_total);
    if (pct == ui_bar_pct) return;
    ui_bar_pct = pct;

    const int bx = 4, by = Y_BAR, bw = UI_W - 8, bh = 9;
    u32 fill = (u32) (((u64) (bw - 4) * pct) / 100);
    DrawRectangle(ALT_SCREEN, bx, by, bw, bh, COLOR_STD_FONT);
    DrawRectangle(ALT_SCREEN, bx + 2, by + 2, bw - 4, bh - 4, COLOR_STD_BG);
    if (fill) DrawRectangle(ALT_SCREEN, bx + 2, by + 2, fill, bh - 4, COLOR_STD_FONT);
}

void FixerUI_Begin(void) {
    ui_active = true;
    ui_phase = FIXERUI_PHASE_OPEN;
    ui_sub_cur = ui_sub_total = 0;
    ui_fixed = ui_bad = 0;
    ui_autoskip = ui_log = ui_refresh_read = false;
    ui_retry = FIXER_CFG_DEFAULT_RETRIES;
    ui_stuck = FIXER_CFG_DEFAULT_STUCK;
    ui_start = ui_last_hb = timer_start();
    ui_last_draw = 0; // force the first Tick to draw immediately
    ui_spin = 0;
    ui_tick_ms = ui_max_tick_ms = 0;
    ui_bar_pct = 0xFFFFFFFFu;
    c_phase[0] = c_sub[0] = c_counts[0] = c_flags[0] = c_limits[0] = c_elapsed[0] = c_status[0] = 0;

    ClearScreen(ALT_SCREEN, COLOR_STD_BG);
    draw_static();
    // The caller sets flags/limits/counts right after Begin and then calls
    // FixerUI_Tick(); drawing here would show stale defaults for one frame.
}

void FixerUI_End(void) {
    if (!ui_active) return;
    ui_active = false;
    ClearScreen(ALT_SCREEN, COLOR_STD_BG);
}

void FixerUI_SetPhase(FixerUiPhase phase) {
    ui_phase = phase;
}

void FixerUI_SetSub(u32 cur, u32 total) {
    ui_sub_cur = cur;
    ui_sub_total = total;
}

void FixerUI_SetCounts(u32 fixed, u32 bad) {
    ui_fixed = fixed;
    ui_bad = bad;
}

void FixerUI_SetFlags(bool autoskip, bool log, bool refresh_every_read) {
    ui_autoskip = autoskip;
    ui_log = log;
    ui_refresh_read = refresh_every_read;
}

void FixerUI_SetLimits(u32 retry, u32 stuck) {
    ui_retry = retry;
    ui_stuck = stuck;
}

void FixerUI_Heartbeat(void) {
    ui_last_hb = timer_start();
}

u32 FixerUI_MaxTickMs(void) {
    return ui_max_tick_ms;
}

void FixerUI_Tick(void) {
    if (!ui_active) return;

    u64 now = timer_start();
    if (timer_msec(ui_last_draw) < FIXERUI_REFRESH_MS) return;
    ui_last_draw = now;
    ui_spin++;

    u64 tick_start = timer_start();

    u64 elapsed = timer_sec(ui_start);
    u64 idle_ms = timer_msec(ui_last_hb);

    draw_cached(4, Y_PHASE, COLOR_STD_FONT, c_phase, "PHASE  %s", phase_name(ui_phase));
    if (ui_sub_total)
        draw_cached(4, Y_SUB, COLOR_LIGHTGREY, c_sub, "       %u / %u", (unsigned) ui_sub_cur, (unsigned) ui_sub_total);
    else
        draw_cached(4, Y_SUB, COLOR_LIGHTGREY, c_sub, "       ...");
    draw_bar();

    draw_cached(4, Y_COUNTS, COLOR_STD_FONT, c_counts, "FIXED %u   UNFIXABLE %u", (unsigned) ui_fixed, (unsigned) ui_bad);
    draw_cached(4, Y_FLAGS, COLOR_LIGHTGREY, c_flags, "Autoskip[%s] Log[%s] Refresh[%s]",
        ui_autoskip ? "ON" : "OFF", ui_log ? "ON" : "OFF", ui_refresh_read ? "ON" : "OFF");
    draw_cached(4, Y_LIMITS, COLOR_LIGHTGREY, c_limits, "Retry %u   Stuck %u",
        (unsigned) ui_retry, (unsigned) ui_stuck);

    static const char spin_chars[] = "|/-\\";
    draw_cached(4, Y_ELAPSED, COLOR_STD_FONT, c_elapsed, "ELAPSED %02u:%02u:%02u  idle %us  %c",
        (unsigned) (elapsed / 3600), (unsigned) ((elapsed / 60) % 60), (unsigned) (elapsed % 60),
        (unsigned) (idle_ms / 1000), spin_chars[ui_spin & 3]);

    if (idle_ms >= 3000)
        draw_cached(4, Y_HELP3, COLOR_RED, c_status, "** NO UPDATE FOR %us **", (unsigned) (idle_ms / 1000));
    else if (c_status[0]) {
        c_status[0] = 0;
        DrawRectangle(ALT_SCREEN, 0, Y_HELP3, UI_W, 9, COLOR_STD_BG);
        DrawString(ALT_SCREEN, "Config set in pre-flight", 4, Y_HELP3, COLOR_DARKGREY, COLOR_STD_BG);
    }

    ui_tick_ms = (u32) timer_msec(tick_start);
    if (ui_tick_ms > ui_max_tick_ms) ui_max_tick_ms = ui_tick_ms;
}

// ---------------------------------------------------------------------------
// Pre-flight screen
// ---------------------------------------------------------------------------

#define PF_ROWS 5

static int preset_index(const u32* arr, int n, u32 val) {
    for (int i = 0; i < n; i++)
        if (arr[i] == val) return i;
    int best = 0;
    u32 bestd = (val > arr[0]) ? (val - arr[0]) : (arr[0] - val);
    for (int i = 1; i < n; i++) {
        u32 d = (val > arr[i]) ? (val - arr[i]) : (arr[i] - val);
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

bool FixerUI_Preflight(const char* path, FixerConfig* cfg) {
    static const u32 retry_presets[] = { 100, 250, 500, 1000, 2500, 10000 };
    static const char* retry_labels[] = { "Fast", "", "Default", "Patient", "Very patient", "Extreme" };
    static const u32 stuck_presets[] = { 25, 50, 100, 200 };
    static const char* stuck_labels[] = { "", "Default", "", "" };
    const int n_retry = (int) (sizeof(retry_presets) / sizeof(retry_presets[0]));
    const int n_stuck = (int) (sizeof(stuck_presets) / sizeof(stuck_presets[0]));
    const u32 lh = GetFontHeight() + 2;
    const int x = 4;

    char buf[96];
    char pathstr[UTF_BUFFER_BYTESIZE(40)];
    TruncateString(pathstr, path, 40, 8);

    int retry_idx = preset_index(retry_presets, n_retry, cfg->retries_before_skip);
    int stuck_idx = preset_index(stuck_presets, n_stuck, cfg->stuck_limit);
    // Snap to the nearest preset so the value shown is the value applied.
    cfg->retries_before_skip = retry_presets[retry_idx];
    cfg->stuck_limit = stuck_presets[stuck_idx];
    int sel = 0;
    bool start = false;

    while (true) {
        int y = 2;
        ClearScreen(MAIN_SCREEN, COLOR_STD_BG);
        DrawString(MAIN_SCREEN, "FIX CARTRIDGE CORRUPTION", x, y, COLOR_STD_FONT, COLOR_STD_BG); y += lh;
        DrawString(MAIN_SCREEN, pathstr, x, y, COLOR_LIGHTGREY, COLOR_STD_BG); y += lh + 4;

        bool on[3] = { cfg->autoskip, cfg->log, cfg->refresh_every_read };
        const char* blabel[3] = { "Autoskip bad blocks", "Write fix report", "Refresh on every read" };
        for (int r = 0; r < 3; r++) {
            snprintf(buf, sizeof(buf), "%s[%c] %s", (sel == r) ? "> " : "  ", on[r] ? 'x' : ' ', blabel[r]);
            DrawString(MAIN_SCREEN, buf, x, y, (sel == r) ? COLOR_STD_FONT : COLOR_LIGHTGREY, COLOR_STD_BG);
            y += lh;
        }
        if (*retry_labels[retry_idx])
            snprintf(buf, sizeof(buf), "%sRetry limit < %u (%s) >",
                (sel == 3) ? "> " : "  ", (unsigned) retry_presets[retry_idx], retry_labels[retry_idx]);
        else
            snprintf(buf, sizeof(buf), "%sRetry limit < %u >",
                (sel == 3) ? "> " : "  ", (unsigned) retry_presets[retry_idx]);
        DrawString(MAIN_SCREEN, buf, x, y, (sel == 3) ? COLOR_STD_FONT : COLOR_LIGHTGREY, COLOR_STD_BG);
        y += lh;
        if (*stuck_labels[stuck_idx])
            snprintf(buf, sizeof(buf), "%sStuck limit < %u (%s) >",
                (sel == 4) ? "> " : "  ", (unsigned) stuck_presets[stuck_idx], stuck_labels[stuck_idx]);
        else
            snprintf(buf, sizeof(buf), "%sStuck limit < %u >",
                (sel == 4) ? "> " : "  ", (unsigned) stuck_presets[stuck_idx]);
        DrawString(MAIN_SCREEN, buf, x, y, (sel == 4) ? COLOR_STD_FONT : COLOR_LIGHTGREY, COLOR_STD_BG);
        y += lh + 4;

        const char* h1 = "";
        const char* h2 = "";
        switch (sel) {
            case 0: h1 = "Skip bad blocks automatically at the"; h2 = "retry limit, else you get a prompt."; break;
            case 1: h1 = "Write gm9/out/fix_report_*.txt with"; h2 = "fixed/unfixable block offsets."; break;
            case 2: h1 = "Refresh on EVERY read. Much slower;"; h2 = "use ONLY as a last resort."; break;
            case 3: h1 = "Re-reads before offering to skip."; h2 = "Autoskip skips at this limit."; break;
            case 4: h1 = "Identical failed reads before a block"; h2 = "is declared unfixable."; break;
        }
        DrawRectangle(MAIN_SCREEN, 0, y, SCREEN_WIDTH_MAIN, 1, COLOR_DARKGREY); y += 3;
        DrawString(MAIN_SCREEN, h1, x, y, COLOR_LIGHTGREY, COLOR_STD_BG); y += lh;
        DrawString(MAIN_SCREEN, h2, x, y, COLOR_LIGHTGREY, COLOR_STD_BG); y += lh + 4;

        DrawString(MAIN_SCREEN, "UP/DOWN select  LEFT/RIGHT change", x, y, COLOR_STD_FONT, COLOR_STD_BG); y += lh;
        DrawString(MAIN_SCREEN, "A start  B cancel  X reset defaults", x, y, COLOR_STD_FONT, COLOR_STD_BG);

        u32 pad = InputWait(0);
        if (pad & BUTTON_UP) sel = (sel + PF_ROWS - 1) % PF_ROWS;
        else if (pad & BUTTON_DOWN) sel = (sel + 1) % PF_ROWS;
        else if (pad & (BUTTON_LEFT | BUTTON_RIGHT)) {
            int dir = (pad & BUTTON_RIGHT) ? 1 : -1;
            switch (sel) {
                case 0: cfg->autoskip = !cfg->autoskip; break;
                case 1: cfg->log = !cfg->log; break;
                case 2: cfg->refresh_every_read = !cfg->refresh_every_read; break;
                case 3: retry_idx = (retry_idx + dir + n_retry) % n_retry; cfg->retries_before_skip = retry_presets[retry_idx]; break;
                case 4: stuck_idx = (stuck_idx + dir + n_stuck) % n_stuck; cfg->stuck_limit = stuck_presets[stuck_idx]; break;
            }
        }
        else if (pad & BUTTON_A) { start = true; break; }
        else if (pad & BUTTON_B) { start = false; break; }
        else if (pad & BUTTON_X) {
            cfg->autoskip = false;
            cfg->log = false;
            cfg->refresh_every_read = false;
            cfg->retries_before_skip = FIXER_CFG_DEFAULT_RETRIES;
            cfg->stuck_limit = FIXER_CFG_DEFAULT_STUCK;
            retry_idx = preset_index(retry_presets, n_retry, cfg->retries_before_skip);
            stuck_idx = preset_index(stuck_presets, n_stuck, cfg->stuck_limit);
        }
    }

    ClearScreen(MAIN_SCREEN, COLOR_STD_BG);
    return start;
}
