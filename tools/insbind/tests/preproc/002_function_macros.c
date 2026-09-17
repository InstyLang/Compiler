#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define SQR(x) ((x) * (x))
#define STR(x) #x
#define CAT(a, b) a ## b
int m = MAX(SQR(3), 2 + 3);
int s = SQR(1 + 2);
const char* t = STR(hello world);
int CAT(va, lue) = 42;
