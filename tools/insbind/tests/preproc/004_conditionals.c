#define FOO 1
#ifdef FOO
int a = 1;
#else
int a = 2;
#endif
#ifndef BAR
int b = 1;
#endif
#if defined(FOO) && !defined(BAR)
int c = 3;
#elif defined(BAR)
int c = 4;
#else
int c = 5;
#endif
#ifdef BAR
# error BAR should not be defined
#endif
