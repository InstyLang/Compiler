typedef unsigned long uLong;
extern uLong compressBound(uLong sourceLen);
struct z_stream_s {
    const char *msg;
    uLong total_in;
};
