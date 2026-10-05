// The fd message shared by vk_macvr.c (which writes and reads it) and the EGL/GLES shim (which reads it):
// one SEQPACKET message holding a header, each shared buffer's flattened AHardwareBuffer, and an optional memfd.
#pragma once
#include <android/hardware_buffer.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#ifndef MAXSH
#define MAXSH 32
#endif
#define MAGIC 0x5852564d   // 'MVRX'
typedef struct { uint32_t magic, count, layers, mips, len[MAXSH], fds[MAXSH], hasGen; } Hdr;

// flatten an AHardwareBuffer the way AHardwareBuffer_sendHandleToUnixSocket does, into data + fds
static int flatten(AHardwareBuffer *b, char *data, size_t cap, int *fds, int *nfd) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    int n = -1;
    if (!AHardwareBuffer_sendHandleToUnixSocket(b, sv[0])) {
        char ctl[CMSG_SPACE(sizeof(int) * 32)];
        struct iovec iov = {data, cap};
        struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof ctl};
        n = recvmsg(sv[1], &m, MSG_CMSG_CLOEXEC);
        *nfd = 0;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                int k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                memcpy(fds + *nfd, CMSG_DATA(c), k * sizeof(int)); *nfd += k;
            }
    }
    close(sv[0]); close(sv[1]);
    return n;
}
// rebuild an AHardwareBuffer from flattened data + fds (dups the fds)
static AHardwareBuffer *unflatten(const char *data, size_t len, const int *fds, int nfd) {
    int sv[2]; AHardwareBuffer *b = NULL;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return NULL;
    char ctl[CMSG_SPACE(sizeof(int) * 32)];
    struct iovec iov = {(void *)data, len};
    struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1};
    if (nfd) {
        m.msg_control = ctl; m.msg_controllen = CMSG_SPACE(sizeof(int) * nfd);
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int) * nfd);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * nfd);
    }
    if (sendmsg(sv[0], &m, 0) == (ssize_t)len) AHardwareBuffer_recvHandleFromUnixSocket(sv[1], &b);
    close(sv[0]); close(sv[1]);
    return b;
}

// build the exported fd from n buffers (+ the counter's memfd, or -1)
static int sendBuffers(AHardwareBuffer **bufs, uint32_t n, uint32_t layers, uint32_t mips, int genFd) {
    static char data[65536]; static int fds[250];
    Hdr *h = (Hdr *)data; memset(h, 0, sizeof *h);
    h->magic = MAGIC; h->count = n; h->layers = layers; h->mips = mips; h->hasGen = genFd >= 0;
    size_t off = sizeof *h; int nfd = 0;
    for (uint32_t i = 0; i < n; i++) {
        int k = 0, got = flatten(bufs[i], data + off, sizeof data - off, fds + nfd, &k);
        if (got <= 0) { for (int j = 0; j < nfd; j++) close(fds[j]); return -1; }
        h->len[i] = got; h->fds[i] = k; off += got; nfd += k;
    }
    if (genFd >= 0) fds[nfd++] = dup(genFd);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { for (int j = 0; j < nfd; j++) close(fds[j]); return -1; }
    char ctl[CMSG_SPACE(sizeof(int) * 250)];
    struct iovec iov = {data, off};
    struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = CMSG_SPACE(sizeof(int) * nfd)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int) * nfd);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * nfd);
    ssize_t w = sendmsg(sv[0], &m, 0);
    for (int j = 0; j < nfd; j++) close(fds[j]);
    close(sv[0]);
    if (w != (ssize_t)off) { close(sv[1]); return -1; }
    return sv[1];
}

// read an exported fd without consuming it
typedef struct { uint32_t n, layers, mips; AHardwareBuffer *buf[MAXSH]; int genFd; } Recv;
static int recvBuffers(int fd, Recv *r) {
    static char data[65536]; char ctl[CMSG_SPACE(sizeof(int) * 250)];
    struct iovec iov = {data, sizeof data};
    struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof ctl};
    ssize_t len = recvmsg(fd, &m, MSG_PEEK | MSG_CMSG_CLOEXEC);
    if (len < (ssize_t)sizeof(Hdr)) return -1;
    int fds[250], nfd = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            memcpy(fds + nfd, CMSG_DATA(c), k * sizeof(int)); nfd += k;
        }
    Hdr *h = (Hdr *)data; int ok = h->magic == MAGIC && h->count <= MAXSH;
    memset(r, 0, sizeof *r); r->genFd = -1;
    size_t off = sizeof *h; int fo = 0;
    for (uint32_t i = 0; ok && i < h->count; i++) {
        r->buf[i] = unflatten(data + off, h->len[i], fds + fo, h->fds[i]);
        ok = r->buf[i] != NULL; off += h->len[i]; fo += h->fds[i];
    }
    if (ok) { r->n = h->count; r->layers = h->layers; r->mips = h->mips; if (h->hasGen) r->genFd = dup(fds[fo]); }
    for (int j = 0; j < nfd; j++) close(fds[j]);
    if (!ok) for (uint32_t i = 0; i < MAXSH; i++) if (r->buf[i]) AHardwareBuffer_release(r->buf[i]);
    return ok ? 0 : -1;
}

