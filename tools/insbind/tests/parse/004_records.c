struct Point { int x, y; };
struct Point origin;
union Bits { int as_int; float as_float; unsigned char bytes[4]; };
struct Node;
struct Node { struct Node *next; int value; };
struct Flags {
    unsigned a : 1;
    unsigned b : 3;
    unsigned : 4;
    unsigned c : 2;
};
typedef struct {
    int code;
    const char *message;
} Error;
struct Outer {
    int tag;
    union {
        int as_int;
        float as_float;
    };
};
struct Aligned { char c; _Alignas(16) int v; };
