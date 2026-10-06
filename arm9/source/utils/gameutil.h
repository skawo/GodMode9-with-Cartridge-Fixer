#pragma once

#include "common.h"

u32 VerifyGameFile(const char* path, bool sig_check);
u32 CheckEncryptedGameFile(const char* path);
u32 CryptGameFile(const char* path, bool inplace, bool encrypt, bool restore);
u32 BuildCiaFromGameFile(const char* path, bool force_legit);
u32 InstallGameFile(const char* path, bool to_emunand);
u32 InstallCifinishFile(const char* path, bool to_emunand);
u32 InstallTicketFile(const char* path, bool to_emunand);
u32 DumpTicketForGameFile(const char* path, bool force_legit);
u32 DumpCxiSrlFromGameFile(const char* path);
u32 ExtractCodeFromCxiFile(const char* path, const char* path_out, char* extstr);
u32 CompressCode(const char* path, const char* path_out);
u64 GetGameFileTrimmedSize(const char* path);
u32 TrimGameFile(const char* path);
u32 ShowGameFileIcon(const char* path, u16* screen);
u32 ShowGameCheckerInfo(const char* path);
u64 GetGameFileTitleId(const char* path);
u32 UninstallGameDataTie(const char* path, bool remove_tie, bool remove_ticket, bool remove_save);
u32 GetTmdContentPath(char* path_content, const char* path_tmd);
u32 GetTieContentPath(char* path_content, const char* path_tie);
u32 BuildNcchInfoXorpads(const char* destdir, const char* path);
u32 CheckHealthAndSafetyInject(const char* hsdrv);
u32 InjectHealthAndSafety(const char* path, const char* destdrv);
u32 BuildTitleKeyInfo(const char* path, bool dec, bool dump);
u32 BuildSeedInfo(const char* path, bool dump);
u32 GetGoodName(char* name, const char* path, bool quick);
// Returned by AttemptFixNcsdFile when the cartridge stopped responding. Chosen so
// it can never collide with the per-region result bits (which are 0..7).
#define FIXRES_CART_STOPPED 0xFFFFFFFFu
// Returned when the cartridge header could not be read (message already shown).
#define FIXRES_HEADER_FAILED 0xFFFFFFFEu
u32 AttemptFixNcsdFile(const char* path, bool log, bool autoskip);

// User-tunable fixer behaviour, set from the pre-flight screen. Defaults match
// the values that were previously hard-coded.
typedef struct {
    bool autoskip;             // skip a bad block automatically at the retry limit
    bool log;                  // write fix_report_*.txt
    bool refresh_every_read;   // send a cartridge refresh on every read (slow)
    u32 retries_before_skip;   // re-reads before offering/auto-skipping a block
    u32 stuck_limit;           // identical failed reads before a block is unfixable
} FixerConfig;

#define FIXER_CFG_DEFAULT_RETRIES 500
#define FIXER_CFG_DEFAULT_STUCK    50
#define FIXER_CFG_MAX_RETRIES      100000
#define FIXER_CFG_MAX_STUCK        10000

// Saved pre-flight settings (remembered across runs).
#define FIXER_CFG_PATH OUTPUT_PATH "/fixer.cfg"
void FixerCfg_Load(FixerConfig* cfg);
void FixerCfg_Save(const FixerConfig* cfg);

extern FixerConfig fixer_cfg;
