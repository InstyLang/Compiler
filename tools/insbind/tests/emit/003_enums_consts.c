#define Z_NO_COMPRESSION 0
#define Z_BEST_SPEED 1
#define Z_BEST_COMPRESSION 9
#define Z_DEFAULT_COMPRESSION (-1)
#define Z_BLOCK (1 << 5)
#define Z_NULL 0
#define VERSION_TEXT "1.3.1"
#define MAX(a, b) ((a) > (b) ? (a) : (b))

enum Level { Low = 1, Mid = 4, High = 16 };
enum Mask { Read = 1 << 0, Write = 1 << 1, Exec = 1 << 2, All = Read | Write | Exec };
enum Negative { ErrA = -3, Ok = 0 };

int zlibVersion(void);
int deflate(int level);
