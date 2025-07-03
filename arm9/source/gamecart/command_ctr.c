// Copyright 2014 Normmatt
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "command_ctr.h"

#include "protocol_ctr.h"
#include "ui.h"

static int read_count = 0;

static int refresh_count = 0;
int refresh_call_every = 10000;
bool force_refresh = false;

//#define HundredRefreshes

static void CTR_CmdC5()
{
    static const u32 c5_cmd[4] = { 0xC5000000, 0x00000000, 0x00000000, 0x00000000 };
    CTR_SendCommand(c5_cmd, 0, 1, 0x100002C, NULL);
}

void CTR_CmdReadData(u32 sector, u32 length, u32 blocks, void* buffer)
{
    if(read_count++ >= refresh_call_every || force_refresh)
    {
        
    #ifdef HundredRefreshes
        for (int i = 0; i < 100; i++)
        {
            refresh_count += 99;
    #endif        
            refresh_count++;
            CTR_CmdC5();
    #ifdef HundredRefreshes
        }
    #endif         

        read_count = 0;

        char tempstr[64];
        snprintf(tempstr, 64, "%d", refresh_count);
        DrawString(MAIN_SCREEN, "Refresh count:", 0, 0, COLOR_STD_FONT, COLOR_STD_BG);
        DrawString(MAIN_SCREEN, tempstr, 0, 10, COLOR_STD_FONT, COLOR_STD_BG);
    }
   
    const u32 read_cmd[4] = {
        (0xBF000000 | (u32)(sector >> 23)),
        (u32)((sector << 9) & 0xFFFFFFFF),
        0x00000000, 0x00000000
    };
    CTR_SendCommand(read_cmd, length, blocks, 0x104822C, buffer);
    
   
    
}

void CTR_CmdReadHeader(void* buffer)
{
    static const u32 readheader_cmd[4] = { 0x82000000, 0x00000000, 0x00000000, 0x00000000 };
    CTR_SendCommand(readheader_cmd, 0x200, 1, 0x704802C, buffer);
}

void CTR_CmdReadUniqueID(void* buffer)
{
    static const u32 readheader_cmd[4] = { 0xC6000000, 0x00000000, 0x00000000, 0x00000000 };
    CTR_SendCommand(readheader_cmd, 0x40, 1, 0x903002C, buffer);
}

u32 CTR_CmdGetSecureId(u32 rand1, u32 rand2)
{
    u32 id = 0;
    const u32 getid_cmd[4] = { 0xA2000000, 0x00000000, rand1, rand2 };
    CTR_SendCommand(getid_cmd, 0x4, 1, 0x701002C, &id);
    return id;
}

void CTR_CmdSeed(u32 rand1, u32 rand2)
{
    const u32 seed_cmd[4] = { 0x83000000, 0x00000000, rand1, rand2 };
    CTR_SendCommand(seed_cmd, 0, 1, 0x700822C, NULL);
}
