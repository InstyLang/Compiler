#define LONG_MACRO(a, \
                   b) ((a) + (b))
int sum = LONG_MACRO(1, \
                     2);
