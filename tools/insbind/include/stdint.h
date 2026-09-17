// insbind freestanding replacement for <stdint.h>.
#ifndef INSBIND_STDINT_H
#define INSBIND_STDINT_H

typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long long int64_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

#if defined(_WIN32) || defined(_WIN64)
typedef long long intptr_t;
typedef unsigned long long uintptr_t;
#else
typedef long intptr_t;
typedef unsigned long uintptr_t;
#endif

#define INT8_MIN (-128)
#define INT8_MAX 127
#define UINT8_MAX 0xffu
#define INT16_MIN (-32768)
#define INT16_MAX 32767
#define UINT16_MAX 0xffffu
#define INT32_MIN (-2147483647 - 1)
#define INT32_MAX 2147483647
#define UINT32_MAX 0xffffffffu
#define INT64_MIN (-9223372036854775807 - 1)
#define INT64_MAX 9223372036854775807
#define UINT64_MAX 0xffffffffffffffffu

#endif /* INSBIND_STDINT_H */
