// insbind freestanding replacement for <stddef.h>.
// Only what library headers actually use; target variants are selected by the
// builtin predefines (_WIN32/_WIN64 vs __linux__), never by the host system.
#ifndef INSBIND_STDDEF_H
#define INSBIND_STDDEF_H

#if defined(_WIN32) || defined(_WIN64)
typedef unsigned long long size_t;
typedef long long ptrdiff_t;
typedef unsigned short wchar_t;
#else
typedef unsigned long size_t;
typedef long ptrdiff_t;
typedef int wchar_t;
#endif

#define NULL ((void*)0)
#define offsetof(type, member) ((size_t)&(((type*)0)->member))

#endif /* INSBIND_STDDEF_H */
