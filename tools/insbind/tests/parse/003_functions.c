int add(int a, int b);
void noargs();
void really_noargs(void);
const char *name_of(int id, const char *fallback);
int (*binary_op(int kind))(int, int);
void (*signal(int sig, void (*handler)(int)))(int);
int printf_like(const char *fmt, ...);
typedef int (*comparator)(const void *a, const void *b);
void sort_with(void *base, unsigned count, unsigned size, comparator cmp);
