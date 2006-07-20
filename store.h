#ifndef _STORE_H
#define _STORE_H

#include <sys/types.h>

#define STORE_MAXBLSZ 65535

struct addr {
    unsigned char hash[32];
};

struct storecache {
    struct addr a;
    void *data;
    ssize_t dlen;
};

struct store {
    struct storeops *ops;
    void *pdata;
    struct storecache *cache;
};

struct storeops {
    int (*put)(struct store *st, const void *buf, size_t len, struct addr *at);
    ssize_t (*get)(struct store *st, void *buf, size_t len, struct addr *at);
    int (*release)(struct store *st);
};

struct store *newstore(struct storeops *ops);
int storeput(struct store *st, const void *buf, size_t len, struct addr *at);
ssize_t storeget(struct store *st, void *buf, size_t len, struct addr *at);
int releasestore(struct store *st);
int addrcmp(struct addr *a1, struct addr *a2);
char *formataddr(struct addr *a);
int parseaddr(char *buf, struct addr *a);
int niladdr(struct addr *a);

struct store *newfstore(char *dir);
int mkfstore(char *dir);

#endif
