// insbind freestanding replacement for <stdarg.h>.
// va_list is ABI-specific: a plain char* on Win64, a struct-array on SysV.
// Only the name matters to binding generation (variadic functions themselves
// are out of scope for the FFI and handled by shims).
#ifndef INSBIND_STDARG_H
#define INSBIND_STDARG_H

#if defined(_WIN32) || defined(_WIN64)
typedef char* va_list;
#else
typedef struct __insbind_va_list_tag* va_list;
#endif

#endif /* INSBIND_STDARG_H */
