#ifndef _BLOCKTREE_H
#define _BLOCKTREE_H

#include "store.h"
#include "inttypes.h"

typedef loff_t block_t;

struct btnode {
    u_int8_t d;
    struct addr a;
};

struct btop {
    block_t blk;
    void *buf;
    size_t len;
    int (*fillfn)(void *buf, size_t len, void *pdata);
    void *pdata;
};

ssize_t btget(struct store *st, struct btnode *tree, block_t bl, void *buf, size_t len, size_t blsize);
int btputmany(struct store *st, struct btnode *tree, struct btop *ops, int numops, size_t blsize);
int btput(struct store *st, struct btnode *tree, block_t bl, void *buf, size_t len, size_t blsize);
block_t btcount(struct store *st, struct btnode *tree, size_t blsize);
void btsortops(struct btop *ops, int numops);
void btmkop(struct btop *op, block_t bl, void *buf, size_t len);

#endif
