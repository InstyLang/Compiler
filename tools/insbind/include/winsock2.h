// insbind freestanding replacement for <winsock2.h>.
// Only what library headers (libcurl being the driver) reference in
// declarations: the SOCKET handle type and its constants. Everything else
// in the real header is a runtime concern of the OS.
#ifndef INSBIND_WINSOCK2_H
#define INSBIND_WINSOCK2_H

typedef unsigned long long SOCKET;
#define INVALID_SOCKET ((unsigned long long)~0)
#define SOCKET_ERROR (-1)

#define FD_SETSIZE 64
typedef struct fd_set {
    unsigned int fd_count;
    SOCKET fd_array[FD_SETSIZE];
} fd_set;

struct timeval {
    long tv_sec;
    long tv_usec;
};

struct sockaddr {
    unsigned short sa_family;
    char sa_data[14];
};

#endif /* INSBIND_WINSOCK2_H */
