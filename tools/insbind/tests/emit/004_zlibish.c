#include <stddef.h>

#define ZEXTERN extern
#define ZEXPORT
#define Z_NULL 0
#define Z_OK 0
#define Z_STREAM_END 1
#define Z_FINISH 4

typedef unsigned long uLong;
typedef void *voidpf;

typedef voidpf (*alloc_func)(voidpf opaque, unsigned items, unsigned size);
typedef void (*free_func)(voidpf opaque, voidpf address);

typedef struct z_stream_s {
    const unsigned char *next_in;
    unsigned avail_in;
    uLong total_in;
    unsigned char *next_out;
    unsigned avail_out;
    const char *msg;
    voidpf opaque;
    alloc_func zalloc;
    free_func zfree;
} z_stream;

ZEXTERN uLong ZEXPORT compressBound(uLong sourceLen);
ZEXTERN int ZEXPORT deflate(z_stream *strm, int flush);
ZEXTERN int ZEXPORT deflateEnd(z_stream *strm);
