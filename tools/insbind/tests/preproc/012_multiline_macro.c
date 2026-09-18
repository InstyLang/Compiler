#define CB(type, name, args) type name args
typedef CB(void, *handler, (int code,
    const char *msg));
handler h;
