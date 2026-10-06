#include "fixersim.h"

#ifdef FIXER_SIM

#include "vff.h"
#include "timer.h"

// Fault injection for testing the corruption fixer on a healthy cartridge.
//
// Enable by building with FIXER_SIM=1 and dropping a config file at
// OUTPUT_PATH "/fixer_sim.cfg", e.g.:
//
//   mode=recover
//   offset=0x001A0000
//   size=0x00040000
//   fail_reads=5
//
// Modes:
//   off       - disabled (default)
//   stuck     - always return corrupt data in range (hash never changes)
//   changing  - corrupt data changes every attempt (hash never matches nor locks)
//   recover   - corrupt for the first `fail_reads` attempts, then return real data
//   fail      - report a read error for the range (simulates a dead cartridge)
//   slow      - wait `delay_ms`, then report a read error
#define SIM_CFG_PATH OUTPUT_PATH "/fixer_sim.cfg"
#define SIM_CFG_MAX  2048

typedef enum {
    SIM_OFF = 0,
    SIM_STUCK,
    SIM_CHANGING,
    SIM_RECOVER,
    SIM_FAIL,
    SIM_SLOW,
} SimMode;

static SimMode sim_mode = SIM_OFF;
static u64 sim_offset = 0;
static u64 sim_size = 0;
static u32 sim_fail_reads = 0;
static u32 sim_delay_ms = 0;
static u32 sim_attempt = 0;
static bool sim_cfg_found = false;

static const char* sim_mode_name(SimMode mode) {
    switch (mode) {
        case SIM_STUCK:    return "stuck";
        case SIM_CHANGING: return "changing";
        case SIM_RECOVER:  return "recover";
        case SIM_FAIL:     return "fail";
        case SIM_SLOW:     return "slow";
        default:           return "off";
    }
}

static SimMode sim_mode_from_str(const char* s) {
    if (!strcmp(s, "stuck"))    return SIM_STUCK;
    if (!strcmp(s, "changing")) return SIM_CHANGING;
    if (!strcmp(s, "recover"))  return SIM_RECOVER;
    if (!strcmp(s, "fail"))     return SIM_FAIL;
    if (!strcmp(s, "slow"))     return SIM_SLOW;
    return SIM_OFF;
}

static u64 sim_parse_num(const char* s) {
    u64 v = 0;
    while ((*s == ' ') || (*s == '\t')) s++;
    if ((s[0] == '0') && ((s[1] == 'x') || (s[1] == 'X'))) {
        s += 2;
        while (isxdigit((unsigned char) *s)) {
            u32 d = isdigit((unsigned char) *s) ? (*s - '0') : (tolower((unsigned char) *s) - 'a' + 10);
            v = (v << 4) | d;
            s++;
        }
    } else {
        while (isdigit((unsigned char) *s)) {
            v = v * 10 + (*s - '0');
            s++;
        }
    }
    return v;
}

static char* sim_trim(char* s) {
    while ((*s == ' ') || (*s == '\t')) s++;
    char* e = s + strlen(s);
    while ((e > s) && ((e[-1] == ' ') || (e[-1] == '\t') || (e[-1] == '\r'))) *--e = 0;
    return s;
}

void FixerSim_LoadConfig(void) {
    FIL file;
    char buf[SIM_CFG_MAX];
    UINT br = 0;

    sim_mode = SIM_OFF;
    sim_offset = sim_size = 0;
    sim_fail_reads = sim_delay_ms = 0;
    sim_cfg_found = false;

    if (fvx_open(&file, SIM_CFG_PATH, FA_READ | FA_OPEN_EXISTING) != FR_OK)
        return;
    sim_cfg_found = true;
    if (fvx_read(&file, buf, sizeof(buf) - 1, &br) != FR_OK) br = 0;
    fvx_close(&file);
    buf[br] = 0;

    char* p = buf;
    while (*p) {
        char* eol = strchr(p, '\n');
        if (eol) *eol = 0;

        char* eq = strchr(p, '=');
        if (eq) {
            *eq = 0;
            char* key = sim_trim(p);
            char* val = sim_trim(eq + 1);
            if (!strcmp(key, "mode"))            sim_mode = sim_mode_from_str(val);
            else if (!strcmp(key, "offset"))     sim_offset = sim_parse_num(val);
            else if (!strcmp(key, "size"))       sim_size = sim_parse_num(val);
            else if (!strcmp(key, "fail_reads")) sim_fail_reads = (u32) sim_parse_num(val);
            else if (!strcmp(key, "delay_ms"))   sim_delay_ms = (u32) sim_parse_num(val);
        }

        if (!eol) break;
        p = eol + 1;
    }

    // Bound the simulated stall so a bad config cannot hang for days.
    if (sim_delay_ms > 60000) sim_delay_ms = 60000;
}

bool FixerSim_Enabled(void) {
    return (sim_mode != SIM_OFF) && (sim_size > 0);
}

const char* FixerSim_ModeName(void) {
    return sim_mode_name(sim_mode);
}

const char* FixerSim_Status(void) {
    if (!sim_cfg_found) return "config NOT FOUND";
    if (sim_mode == SIM_OFF) return "found, mode=off";
    if (sim_size == 0) return "found, size=0 (inactive)";
    return sim_mode_name(sim_mode);
}

void FixerSim_BeginUnit(u64 offset, u32 size) {
    (void) offset;
    (void) size;
    sim_attempt = 0;
}

void FixerSim_BeginAttempt(void) {
    if (sim_attempt < (u32) -1) sim_attempt++;
}

static void sim_corrupt(void* buffer, u32 size, u32 seed) {
    u8* b = (u8*) buffer;
    u32 pat = 0x5A5A5A5Au ^ seed;
    for (u32 i = 0; i < size; i++)
        b[i] ^= (u8) (pat >> ((i & 3) * 8));
}

int FixerSim_FilterRead(int fr, void* buffer, u32 size, u64 abs_offset) {
    if (!FixerSim_Enabled())
        return fr;

    // Never mask a real read error with simulated corruption.
    if (fr != 0)
        return fr;

    // byte range of this read overlapping the configured fault range?
    if ((abs_offset + size <= sim_offset) || (abs_offset >= sim_offset + sim_size))
        return fr;

    switch (sim_mode) {
        case SIM_STUCK:
            sim_corrupt(buffer, size, 0);
            return 0;
        case SIM_CHANGING:
            sim_corrupt(buffer, size, sim_attempt);
            return 0;
        case SIM_RECOVER:
            // Changing seed so the fixer's "hash stuck" heuristic cannot give up
            // before `fail_reads` attempts have elapsed.
            if (sim_attempt <= sim_fail_reads)
                sim_corrupt(buffer, size, sim_attempt);
            return 0;
        case SIM_FAIL:
            return 1;
        case SIM_SLOW:
            wait_msec(sim_delay_ms);
            return 1;
        default:
            return fr;
    }
}

#endif // FIXER_SIM
