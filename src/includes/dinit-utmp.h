// Wrappers for utmp/wtmp & equivalent database access.

#ifndef DINIT_UTMP_H_INCLUDED
#define DINIT_UTMP_H_INCLUDED

#ifndef USE_UTMPX
#define USE_UTMPX 1
#endif

#ifndef USE_UPDWTMPX
#ifdef __linux__
#define USE_UPDWTMPX 1
#else
#define USE_UPDWTMPX 0
#endif
#endif

#if USE_UTMPX

#include <cstring>

#include <utmpx.h>
#include <sys/time.h>

// Set the time for a utmpx record to the current time.
inline void set_current_time(struct utmpx *record)
{
#ifdef __linux__
    // On Linux, ut_tv is not actually a struct timeval:
    timeval curtime;
    gettimeofday(&curtime, nullptr);
    record->ut_tv.tv_sec = curtime.tv_sec;
    record->ut_tv.tv_usec = curtime.tv_usec;
#else
    gettimeofday(&record->ut_tv, nullptr);
#endif
}

// Log the boot time to the wtmp database (or equivalent).
inline bool log_boot()
{
    struct utmpx record;
    memset(&record, 0, sizeof(record));
    record.ut_type = BOOT_TIME;

    set_current_time(&record);

    // On FreeBSD, putxline will update all appropriate databases. On Linux, it only updates
    // the utmp database: we need to update the wtmp database explicitly:
#if USE_UPDWTMPX
    updwtmpx(_PATH_WTMPX, &record);
#endif

    setutxent();
    bool success = (pututxline(&record) != NULL);
    endutxent();

    return success;
}

#else // Don't update databases:

static inline void log_boot()  {  }

#endif

#endif
