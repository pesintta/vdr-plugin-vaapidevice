//////////////////////////////////////////////////////////////////////////////
///
///	Copyright (c) 2009 - 2012 by Lutz Sammer.  All Rights Reserved.
///
///	Contributor(s):
///		Copied from uwm.
///
///	License: AGPLv3
///
///	This program is free software: you can redistribute it and/or modify
///	it under the terms of the GNU Affero General Public License as
///	published by the Free Software Foundation, either version 3 of the
///	License.
///
///	This program is distributed in the hope that it will be useful,
///	but WITHOUT ANY WARRANTY; without even the implied warranty of
///	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
///	GNU Affero General Public License for more details.
///
//////////////////////////////////////////////////////////////////////////////

/// @addtogroup misc
/// @{

#include <syslog.h>
#include <stdarg.h>
#include <time.h>			// clock_gettime

//////////////////////////////////////////////////////////////////////////////
//	Defines
//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
//	Declares
//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
//	Variables
//////////////////////////////////////////////////////////////////////////////

extern int LogLevel;			///< how much information wanted

//////////////////////////////////////////////////////////////////////////////
//	Prototypes
//////////////////////////////////////////////////////////////////////////////

static inline void Syslog(const int, const char *format, ...)
    __attribute__ ((format(printf, 2, 3)));

//////////////////////////////////////////////////////////////////////////////
//	Inlines
//////////////////////////////////////////////////////////////////////////////

#ifdef DEBUG
#define DebugLevel 4			/// private debug level
#else
#define DebugLevel 0			/// private debug level
#endif

/**
**	Syslog output function.
**
**	- 0	fatal errors and errors
**	- 1	warnings
**	- 2	info
**	- 3	important debug and fixme's
*/
static inline void Syslog(const int level, const char *format, ...)
{
    if (LogLevel > level || DebugLevel > level) {
	va_list ap;
	int priority = LOG_DEBUG;
	switch (level) {
	    case 0:  priority = LOG_ERR;     break;
	    case 1:  priority = LOG_WARNING; break;
	    case 2:  priority = LOG_INFO;    break;
	    default: priority = LOG_DEBUG;   break;
	}

	va_start(ap, format);
	vsyslog(priority, format, ap);
	va_end(ap);
    }
}

/**
**	Show error.
*/
#define Error(fmt...)	Syslog(0, fmt)

/**
**	Show fatal error.
*/
#define Fatal(fmt...)	do { Error(fmt); abort(); } while (0)

/**
**	Show warning.
*/
#define Warning(fmt...)	Syslog(1, fmt)

/**
**	Show info.
*/
#define Info(fmt...)	Syslog(2, fmt)

/**
**	Show debug.
*/
#ifdef DEBUG
#define Debug(level, fmt...)	Syslog(level, fmt)
#else
#define Debug(level, fmt...)		/* disabled */
#endif

#ifndef AV_NOPTS_VALUE
#define AV_NOPTS_VALUE INT64_C(0x8000000000000000)
#endif

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
    snprintf(buf[idx], sizeof(buf[idx]), "%2d:%02d:%02d.%03d",
	(int)(ts / (90 * 3600000)), (int)((ts / (90 * 60000)) % 60),
	(int)((ts / (90 * 1000)) % 60), (int)((ts / 90) % 1000));

    return buf[idx];
}

/**
**	Get ticks in us.
**
**	@returns ticks in us,
*/
static inline uint32_t GetUsTicks(void)
{
#ifdef CLOCK_MONOTONIC
    struct timespec tspec;

    clock_gettime(CLOCK_MONOTONIC, &tspec);
    return (tspec.tv_sec * 1000 * 1000) + (tspec.tv_nsec / (1000));
#else
    struct timeval tval;

    if (gettimeofday(&tval, NULL) < 0) {
	return 0;
    }
    return (tval.tv_sec * 1000 * 1000) + (tval.tv_usec);
#endif
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

/// @}
