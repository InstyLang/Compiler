#define WIDTH 80
#define HEIGHT (WIDTH / 2)
int a = HEIGHT;      // expands to (80 / 2)
#undef WIDTH
#define WIDTH 100
int b = HEIGHT;      // macros bind at use site: (100 / 2)
