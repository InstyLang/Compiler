// insbind freestanding replacement for <sys/types.h>.
// Types match the C runtime the target actually links: MSVCRT semantics on
// Windows (off_t is 32-bit long), POSIX on Linux.
#ifndef INSBIND_SYS_TYPES_H
#define INSBIND_SYS_TYPES_H

#if defined(_WIN32) || defined(_WIN64)
typedef long off_t;
typedef long long off64_t;
#else
typedef long off_t;
typedef long long off64_t;
#endif

typedef int pid_t;
typedef unsigned int mode_t;

#endif /* INSBIND_SYS_TYPES_H */
