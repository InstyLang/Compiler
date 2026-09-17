struct Point { int x, y; };
union Bits { int as_int; float as_float; unsigned char bytes[4]; };
struct Flags {
    unsigned a : 1;
    unsigned b : 3;
    unsigned : 4;
    unsigned c : 2;
};
struct Node { struct Node *next; int value; };
struct Aligned { char c; _Alignas(16) int v; };
struct Outer {
    int tag;
    union {
        int as_int;
        float as_float;
    };
};
