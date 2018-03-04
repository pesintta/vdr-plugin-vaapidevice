/// Copyright (C) 2009 - 2012 by Johns. All Rights Reserved.
/// Copyright (C) 2018 by pesintta, rofafor.
///
/// SPDX-License-Identifier: AGPL-3.0-only

#include <syslog.h>
#include <stdarg.h>
#include <time.h>			// clock_gettime

//////////////////////////////////////////////////////////////////////////////
//  Defines
//////////////////////////////////////////////////////////////////////////////

#ifndef AV_NOPTS_VALUE
#define AV_NOPTS_VALUE INT64_C(0x8000000000000000)
#endif

//////////////////////////////////////////////////////////////////////////////
//  Declares
//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
//  Variables
//////////////////////////////////////////////////////////////////////////////

extern int TraceMode;			///< trace mode for debugging

//////////////////////////////////////////////////////////////////////////////
//  Prototypes
//////////////////////////////////////////////////////////////////////////////

extern void LogMessage(int trace, int level, const char *format, ...) __attribute__ ((format(printf, 3, 4)));

//////////////////////////////////////////////////////////////////////////////
//  Inlines
//////////////////////////////////////////////////////////////////////////////

#define Fatal(a...)   do { LogMessage(0, 0, a); abort(); } while (0)
#define Error(a...)   LogMessage(0,  0, a)
#define Info(a...)    LogMessage(0,  1, a)
#define Debug(a...)   LogMessage(0,  3, a)
#define Debug1(a...)  LogMessage(1,  2, a)  // Device
#define Debug2(a...)  LogMessage(2,  2, a)  // X11
#define Debug3(a...)  LogMessage(3,  2, a)  // Demuxer
#define Debug4(a...)  LogMessage(4,  2, a)  // Codec
#define Debug5(a...)  LogMessage(5,  2, a)  // Audio
#define Debug6(a...)  LogMessage(6,  2, a)  // Audio: extra
#define Debug7(a...)  LogMessage(7,  2, a)  // Video
#define Debug8(a...)  LogMessage(8,  2, a)  // Video: extra
#define Debug9(a...)  LogMessage(9,  2, a)  // FFMPEG: verbose
#define Debug10(a...) LogMessage(10, 2, a)  // FFMPEG: info
#define Debug11(a...) LogMessage(11, 2, a)  // FFMPEG: warning
#define Debug12(a...) LogMessage(12, 2, a)  // FFMPEG: error
#define Debug13(a...) LogMessage(13, 2, a)  // TBD
#define Debug14(a...) LogMessage(14, 2, a)  // TBD
#define Debug15(a...) LogMessage(14, 2, a)  // TBD
#define Debug16(a...) LogMessage(16, 2, a)  // TBD

/**
**	Nice time-stamp string.
**
**	@param ts	dvb time stamp
*/
static inline const char *Timestamp2String(int64_t ts)
{
    static char buf[4][16];
    static int idx;

    if (ts == (int64_t) AV_NOPTS_VALUE) {
	return "--:--:--.---";
    }
    idx = (idx + 1) % 3;
    snprintf(buf[idx], sizeof(buf[idx]), "%2d:%02d:%02d.%03d", (int)(ts / (90 * 3600000)),
	(int)((ts / (90 * 60000)) % 60), (int)((ts / (90 * 1000)) % 60), (int)((ts / 90) % 1000));

    return buf[idx];
}

/**
**	Get ticks in ns.
**
**	@returns ticks in ns,
*/
static inline uint64_t GetNsTicks(void)
{
    struct timespec tspec;

    clock_gettime(CLOCK_MONOTONIC, &tspec);
    return (tspec.tv_sec * 1000 * 1000 * 1000) + tspec.tv_nsec;
}

/**
**	Get ticks in us.
**
**	@returns ticks in us,
*/
static inline uint32_t GetUsTicks(void)
{
    return GetNsTicks() / 1000;
}

/**
**	Get ticks in ms.
**
**	@returns ticks in ms,
*/
static inline uint32_t GetMsTicks(void)
{
    return GetUsTicks() / 1000;
}
