#pragma once

#include "common.h"

// Cartridge fault injection for testing the fixer without a defective cartridge.
// Enabled by building with FIXER_SIM=1 and controlled at runtime by a config file
// on the SD card. In normal builds every call below is a no-op.

#ifdef FIXER_SIM
void FixerSim_LoadConfig(void);
bool FixerSim_Enabled(void);
const char* FixerSim_ModeName(void);
const char* FixerSim_Status(void);
void FixerSim_BeginUnit(u64 offset, u32 size);
void FixerSim_BeginAttempt(void);
int FixerSim_FilterRead(int fr, void* buffer, u32 size, u64 abs_offset);
#else
static inline void FixerSim_LoadConfig(void) {}
static inline bool FixerSim_Enabled(void) { return false; }
static inline const char* FixerSim_ModeName(void) { return ""; }
static inline const char* FixerSim_Status(void) { return ""; }
static inline void FixerSim_BeginUnit(u64 offset, u32 size) { (void) offset; (void) size; }
static inline void FixerSim_BeginAttempt(void) {}
static inline int FixerSim_FilterRead(int fr, void* buffer, u32 size, u64 abs_offset) {
    (void) buffer; (void) size; (void) abs_offset; return fr;
}
#endif
