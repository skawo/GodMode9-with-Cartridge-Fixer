// Copyright 2014 Normmatt
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <arm.h>

#include "protocol_ctr.h"

#include "protocol.h"
#include "timer.h"
#ifdef VERBOSE_COMMANDS
#include "draw.h"
#endif

// Upper bound for a single CTR card command. A 1MB transfer at 13.4 MHz takes
// roughly 0.6s, so this is generous. It exists so a dead/failing cartridge can
// no longer spin the CPU forever (which froze the screen and ignored B/Y).
#define CTR_CMD_TIMEOUT_MS 5000

// Stall watchdog. `base` is restarted whenever forward progress is observed, so
// a slow-but-progressing transfer is never aborted while a genuine stall (no
// progress) still trips after CTR_CMD_TIMEOUT_MS.
typedef struct {
    u64 base;
    u32 spins;
    u32 last_count;
} CtrTimeout;

static inline bool ctr_timeout_hit(CtrTimeout* t, u32 count) {
    if (t->spins++ & 0xFFF)
        return false;
    if (count != t->last_count) {
        t->last_count = count;
        t->base = timer_start();
        return false;
    }
    u64 ms = timer_msec(t->base);
    if (ms > 1000000) { // timer was reset / wrapped: re-arm, never false-trigger
        t->base = timer_start();
        return false;
    }
    return (ms >= CTR_CMD_TIMEOUT_MS);
}

bool CTR_SetSecKey(u32 value) {
    REG_CTRCARDSECCNT |= ((value & 3) << 8) | 4;
    CtrTimeout to = { timer_start(), 0, 0 };
    while (!(REG_CTRCARDSECCNT & 0x4000)) {
        if (ctr_timeout_hit(&to, to.last_count)) return false;
    }
    return true;
}

bool CTR_SetSecSeed(const u32* seed, bool flag) {
    REG_CTRCARDSECSEED = BSWAP32(seed[3]);
    REG_CTRCARDSECSEED = BSWAP32(seed[2]);
    REG_CTRCARDSECSEED = BSWAP32(seed[1]);
    REG_CTRCARDSECSEED = BSWAP32(seed[0]);
    REG_CTRCARDSECCNT |= 0x8000;

    CtrTimeout to = { timer_start(), 0, 0 };
    while (!(REG_CTRCARDSECCNT & 0x4000)) {
        if (ctr_timeout_hit(&to, to.last_count)) return false;
    }

    if (flag) {
        (*(vu32*)0x1000400C) = 0x00000001; // Enable cart command encryption?
    }
    return true;
}

bool CTR_SendCommand(const u32 command[4], u32 pageSize, u32 blocks, u32 latency, void* buffer)
{
#ifdef VERBOSE_COMMANDS
    Debug("C> %08X %08X %08X %08X", command[0], command[1], command[2], command[3]);
#endif

    REG_CTRCARDCMD[0] = command[3];
    REG_CTRCARDCMD[1] = command[2];
    REG_CTRCARDCMD[2] = command[1];
    REG_CTRCARDCMD[3] = command[0];

    //Make sure this never happens
    if(blocks == 0) blocks = 1;

    pageSize -= pageSize & 3; // align to 4 byte
    u32 pageParam = CTRCARD_PAGESIZE_4K;
    u32 transferLength = 4096;
    // make zero read and 4 byte read a little special for timing optimization(and 512 too)
    switch(pageSize) {
        case 0:
            transferLength = 0;
            pageParam = CTRCARD_PAGESIZE_0;
            break;
        case 4:
            transferLength = 4;
            pageParam = CTRCARD_PAGESIZE_4;
            break;
        case 64:
            transferLength = 64;
            pageParam = CTRCARD_PAGESIZE_64;
            break;
        case 512:
            transferLength = 512;
            pageParam = CTRCARD_PAGESIZE_512;
            break;
        case 1024:
            transferLength = 1024;
            pageParam = CTRCARD_PAGESIZE_1K;
            break;
        case 2048:
            transferLength = 2048;
            pageParam = CTRCARD_PAGESIZE_2K;
            break;
        case 4096:
            transferLength = 4096;
            pageParam = CTRCARD_PAGESIZE_4K;
            break;
        default:
            break; //Defaults already set
    }

    REG_CTRCARDBLKCNT = blocks - 1;
    transferLength *= blocks;

    // go
    REG_CTRCARDCNT = 0x10000000;
    REG_CTRCARDCNT = /*CTRKEY_PARAM | */CTRCARD_ACTIVATE | CTRCARD_nRESET | pageParam | latency;

    u8 * pbuf = (u8 *)buffer;
    u32 * pbuf32 = (u32 * )buffer;
    bool useBuf = ( NULL != pbuf );
    bool useBuf32 = (useBuf && (0 == (3 & ((u32)buffer))));

    u32 count = 0;
    u32 cardCtrl = REG_CTRCARDCNT;
    CtrTimeout to = { timer_start(), 0, 0 };
    bool timed_out = false;

    if(useBuf32)
    {
        while( (cardCtrl & CTRCARD_BUSY) && count < transferLength)
        {
            cardCtrl = REG_CTRCARDCNT;
            if( cardCtrl & CTRCARD_DATA_READY  ) {
                u32 data = REG_CTRCARDFIFO;
                *pbuf32++ = data;
                count += 4;
            } else if (ctr_timeout_hit(&to, count)) {
                timed_out = true;
                break;
            }
        }
    }
    else if(useBuf)
    {
        while( (cardCtrl & CTRCARD_BUSY) && count < transferLength)
        {
            cardCtrl = REG_CTRCARDCNT;
            if( cardCtrl & CTRCARD_DATA_READY  ) {
                u32 data = REG_CTRCARDFIFO;
                pbuf[0] = (unsigned char) (data >>  0);
                pbuf[1] = (unsigned char) (data >>  8);
                pbuf[2] = (unsigned char) (data >> 16);
                pbuf[3] = (unsigned char) (data >> 24);
                pbuf += sizeof (unsigned int);
                count += 4;
            } else if (ctr_timeout_hit(&to, count)) {
                timed_out = true;
                break;
            }
        }
    }
    else
    {
        while( (cardCtrl & CTRCARD_BUSY) && count < transferLength)
        {
            cardCtrl = REG_CTRCARDCNT;
            if( cardCtrl & CTRCARD_DATA_READY  ) {
                u32 data = REG_CTRCARDFIFO;
                (void)data;
                count += 4;
            } else if (ctr_timeout_hit(&to, count)) {
                timed_out = true;
                break;
            }
        }
    }

    // if read is not finished, ds will not pull ROM CS to high, we pull it high manually
    if( !timed_out && count != transferLength ) {
        // MUST wait for next data ready,
        // if ds pull ROM CS to high during 4 byte data transfer, something will mess up
        // so we have to wait next data ready
        // (re-read the register first: the value left by the transfer loop is stale)
        do {
            cardCtrl = REG_CTRCARDCNT;
            if (!(cardCtrl & CTRCARD_DATA_READY) && ctr_timeout_hit(&to, to.last_count)) {
                timed_out = true;
                break;
            }
        } while (!(cardCtrl & CTRCARD_DATA_READY));
        // and this tiny delay is necessary
        ARM_WaitCycles(33 * 8);
        // pull ROM CS high (this also aborts a stalled command)
        REG_CTRCARDCNT = 0x10000000;
        REG_CTRCARDCNT = CTRKEY_PARAM | CTRCARD_ACTIVATE | CTRCARD_nRESET;
    }
    // wait rom cs high (bounded)
    cardCtrl = REG_CTRCARDCNT;
    while( cardCtrl & CTRCARD_BUSY ) {
        cardCtrl = REG_CTRCARDCNT;
        if (ctr_timeout_hit(&to, to.last_count)) {
            timed_out = true;
            break;
        }
    }
    if (timed_out) {
        // force-release the bus so the next command isn't issued while it's stuck
        REG_CTRCARDCNT = 0x10000000;
    }
    //lastCmd[0] = command[0];lastCmd[1] = command[1];

#ifdef VERBOSE_COMMANDS
    if (!useBuf) {
        Debug("C< NULL");
    } else if (!useBuf32) {
        Debug("C< non32");
    } else {
        u32* p = (u32*)buffer;
        int transferWords = count / 4;
        for (int i = 0; i < transferWords && i < 4*4; i += 4) {
            switch (transferWords - i) {
            case 0:
                break;
            case 1:
                Debug("C< %08X", p[i+0]);
                break;
            case 2:
                Debug("C< %08X %08X", p[i+0], p[i+1]);
                break;
            case 3:
                Debug("C< %08X %08X %08X", p[i+0], p[i+1], p[i+2]);
                break;
            default:
                Debug("C< %08X %08X %08X %08X", p[i+0], p[i+1], p[i+2], p[i+3]);
                break;
            }
        }
    }
#endif

    // Only a genuine stall (watchdog) counts as a failure. A short/early-cleared
    // transfer is the pre-existing "pull CS high" case that the original code
    // always treated as success; flagging it broke secure-init and verify.
    return !timed_out;
}
