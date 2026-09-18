// insbind freestanding replacement for <time.h>.
// Only what library headers reference in declarations.
#ifndef INSBIND_TIME_H
#define INSBIND_TIME_H

#if defined(_WIN32) || defined(_WIN64)
typedef long long time_t;
#else
typedef long time_t;
#endif

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

#endif /* INSBIND_TIME_H */
