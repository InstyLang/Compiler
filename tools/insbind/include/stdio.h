// insbind freestanding replacement for <stdio.h>.
// Only what binding generation needs: FILE as an opaque handle and the seek
// constants. No function declarations -- stdio *functions* are a runtime
// concern, and bindings for memory-based I/O never call them.
#ifndef INSBIND_STDIO_H
#define INSBIND_STDIO_H

#ifndef INSBIND_STDDEF_H
#include <stddef.h>
#endif

typedef struct _iobuf FILE;

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define EOF (-1)

#endif /* INSBIND_STDIO_H */
