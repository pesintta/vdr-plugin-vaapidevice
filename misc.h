///
///	@file misc.h	@brief Misc function header file
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
///	$Id$
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

extern int SysLogLevel;			///< how much information wanted

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
    if (SysLogLevel > level || DebugLevel > level) {
	va_list ap;

	va_start(ap, format);
	vsyslog(LOG_ERR, format, ap);
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
#define Fatal(fmt...)	do { Error(fmt); exit(-1); } while (0)

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

/**
**	Get ticks in ms.
**
**	@returns ticks in ms,
*/
static inline uint32_t GetMsTicks(void)
{
#ifdef CLOCK_MONOTONIC
    struct timespec tspec;

    clock_gettime(CLOCK_MONOTONIC, &tspec);
    return (tspec.tv_sec * 1000) + (tspec.tv_nsec / (1000 * 1000));
#else
    struct timeval tval;

    if (gettimeofday(&tval, NULL) < 0) {
	return 0;
    }
    return (tval.tv_sec * 1000) + (tval.tv_usec / 1000);
#endif
}

/// @}
