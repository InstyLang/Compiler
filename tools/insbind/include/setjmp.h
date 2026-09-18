// insbind freestanding replacement for <setjmp.h>.
// Present only so headers that #include <setjmp.h> (libpng's pngconf.h is
// the canonical example) process cleanly. No public libpng declaration uses
// jmp_buf; the macro-only uses (png_jmpbuf) never expand in binding
// generation. The RUNTIME story -- error handling without setjmp -- belongs
// to the package's docs and shim layer, not to this file.
#ifndef INSBIND_SETJMP_H
#define INSBIND_SETJMP_H

typedef int jmp_buf[16]; // opaque placeholder, never dereferenced

#endif /* INSBIND_SETJMP_H */
