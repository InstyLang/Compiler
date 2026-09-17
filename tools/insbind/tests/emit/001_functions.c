typedef unsigned long uLong;
typedef int (*comparator)(const void *a, const void *b);

extern int add(int a, int b);
const char *name_of(int id);
char *buffer_of(int slot);
void *opaque_handle(void);
uLong compressBound(uLong sourceLen);
void sort_with(void *base, unsigned count, unsigned size, comparator cmp);
int printf_like(const char *fmt, ...);
static inline int max2(int a, int b) { return a > b ? a : b; }
extern int errno_like_global;
