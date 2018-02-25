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

extern void log_message(int trace, int level, const char *format, ...) __attribute__ ((format(printf, 3, 4)));

//////////////////////////////////////////////////////////////////////////////
//  Inlines
//////////////////////////////////////////////////////////////////////////////

#define Fatal(a...)  do { log_message(0, 0, a); abort(); } while (0)
#define Error(a...) log_message(0, 0, a)
#define Info(a...) log_message(0, 1, a)
#define Debug(trace, a...) log_message(trace, 2, a)

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
