typedef int A, *B, C[4];
typedef unsigned long uLong;
typedef uLong uLongLongDistance;
typedef int Handler(int);
typedef Handler *HandlerPtr;
typedef const char *const *StringTable;
uLong distance(uLongLongDistance extra, HandlerPtr on_done);
