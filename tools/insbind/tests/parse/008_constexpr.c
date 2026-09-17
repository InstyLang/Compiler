#define BLOCK_SIZE 4096
#define HALF_BLOCK (BLOCK_SIZE / 2)
enum Level { Low = 1, Mid = 4, High = 16 };
int buf1[BLOCK_SIZE];
int buf2[HALF_BLOCK + 8];
int buf3[High * 2];
int buf4[1 << 10];
int buf5['A'];
enum Mask { Read = 1 << 0, Write = 1 << 1, Exec = 1 << 2, All = Read | Write | Exec };
enum Neg { Down = -1, Up = Down > 0 ? 2 : 1 };
struct S { unsigned flags : (1 + 3); char pad[High - Mid]; };
