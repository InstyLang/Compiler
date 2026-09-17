#if 1 + 2 * 3 == 7
int a = 1;
#endif
#if (4 >> 1) == 2 && (1 << 3) == 8
int b = 1;
#endif
#if 10 % 3 == 1 && 10 / 3 == 3
int c = 1;
#endif
#if 'A' == 65 && '\n' == 10
int d = 1;
#endif
#if 0x10 == 16 && 010 == 8
int e = 1;
#endif
#if 1 ? 0 : 1
#error ternary broken
#endif
#if 5 > 3 ? 1 : 0
int f = 1;
#endif
#if 2 == 2 == 1
int g = 1;
#endif
#if -1 < 0
int h = 1;
#endif
#if ~0 == -1
int i = 1;
#endif
#if 0xffffffffu > 1
int j = 1;
#endif
