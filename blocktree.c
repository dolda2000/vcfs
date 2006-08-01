#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "store.h"
#include "blocktree.h"

#define min(a, b) (((b) < (a))?(b):(a))
#define ISDELOP(op) (((op).buf == NULL) && ((op).fillfn == NULL))

ssize_t btget(struct store *st, struct btnode *tree, block_t bl, void *buf, size_t len)
{
    int d;
    block_t c, sel;
    struct btnode indir[BT_INDSZ];
    ssize_t sz;
    
    if(tree->d == 0) {
	errno = ERANGE;
	return(-1);
    }
    while(1) {
	d = tree->d & 0x7f;
	/* This check should really only be necessary on the first
	 * iteration, but I felt it was easier to put it in the
	 * loop. */
	if((bl >> (d * BT_INDBITS)) > 0) {
	    errno = ERANGE;
	    return(-1);
	}
	
	if(d == 0)
	    return(storeget(st, buf, len, &tree->a));
	
	/* Luckily, this is tail recursive */
	if((sz = storeget(st, indir, BT_INDBSZ, &tree->a)) < 0)
	    return(-1);
	c = sz / sizeof(struct btnode);
	sel = bl >> ((d - 1) * BT_INDBITS);
	if(sel >= c) {
	    errno = ERANGE;
	    return(-1);
	}
	tree = &indir[sel];
	bl &= (1LL << ((d - 1) * BT_INDBITS)) - 1;
    }
    return(0);
}

static int btputleaf(struct store *st, struct btnode *leaf, struct btop *op, block_t bloff)
{
    void *buf;
    struct addr na;
    int ret;
    
    if(ISDELOP(*op)) {
	leaf->d = 0;
	return(0);
    }
    buf = NULL;
    if(op->buf == NULL) {
	buf = op->buf = malloc(op->len);
	if(op->fillfn(buf, op->len, op->pdata))
	    return(-1);
    }
    ret = storeput(st, op->buf, op->len, &na);
    if(buf != NULL)
	free(buf);
    if(ret)
	return(-1);
    leaf->d = 0x80;
    leaf->a = na;
    return(0);
}

static int countops(struct btop *ops, int numops, block_t bloff, block_t maxbl)
{
    int i;
    
    for(i = 0; i < numops; i++) {
	if(ops[i].blk - bloff >= maxbl)
	    break;
    }
    return(i);
}

/*
 * blputmany() in many ways makes the code uglier, but it saves a
 * *lot* of space, since it doesn't need to store intermediary blocks.
 */
static int btputmany2(struct store *st, struct btnode *tree, struct btop *ops, int numops, block_t bloff)
{
    int i, subops, d, f, hasid;
    block_t c, sel, bl, nextsz;
    struct addr na;
    struct btnode indir[BT_INDSZ];
    ssize_t sz;
    
    d = tree->d & 0x7f;
    f = tree->d & 0x80;

    hasid = 0;
    
    for(i = 0; i < numops; ) {
	if(ops[i].blk < bloff) {
	    errno = ERANGE;
	    return(-1);
	}
	bl = ops[i].blk - bloff;
    
	if((d == 0) && (bl == 0)) {
	    if(btputleaf(st, tree, ops, bloff))
		return(-1);
	    i++;
	    continue;
	}
    
	if(f && (bl == (1LL << (d * BT_INDBITS)))) {
	    /* New level of indirection */
	    if(hasid) {
		if(storeput(st, indir, c * sizeof(struct btnode), &na))
		    return(-1);
		tree->a = na;
	    }
	    indir[0] = *tree;
	    tree->d = ++d;
	    f = 0;
	    c = 1;
	    hasid = 1;
	} else if(d == 0) {
	    /* New tree */
	    if(bl != 0) {
		errno = ERANGE;
		return(-1);
	    }
	    /* Assume that numops == largest block number + 1 -- gaps
	     * will be detected as errors later */
	    for(bl = numops - 1; bl > 0; d++, bl <<= BT_INDBITS);
	    tree->d = d;
	    c = 0;
	    hasid = 1;
	} else {
	    /* Get indirect block */
	    if(!hasid) {
		if((sz = storeget(st, indir, BT_INDBSZ, &tree->a)) < 0)
		    return(-1);
		c = sz / sizeof(struct btnode);
		hasid = 1;
	    }
	}

	sel = bl >> ((d - 1) * BT_INDBITS);
	if(sel > c) {
	    errno = ERANGE;
	    return(-1);
	}
    
	if(sel == c) {
	    /* Append new */
	    if((c > 0) && (!(indir[c - 1].d & 0x80) || ((indir[c - 1].d & 0x7f) < (d - 1)))) {
		errno = ERANGE;
		return(-1);
	    }
	    indir[c].d = 0;
	    c++;
	}
	nextsz = 1LL << ((d - 1) * BT_INDBITS);
	subops = countops(ops + i, numops - i, bloff + (sel * nextsz), nextsz);
	if(btputmany2(st, &indir[sel], ops + i, subops, bloff + (sel * nextsz)))
	    return(-1);
	i += subops;
	
	if((sel == BT_INDSZ - 1) && (indir[sel].d == ((d - 1) | 0x80))) {
	    /* Filled up */
	    tree->d |= 0x80;
	    f = 1;
	} else if(indir[sel].d == 0) {
	    /* Erased */
	    if(--c == 1) {
		tree->d = indir[0].d;
		tree->a = indir[0].a;
	    }
	}
    }
    if(hasid) {
	if(storeput(st, indir, c * sizeof(struct btnode), &na))
	    return(-1);
	tree->a = na;
    }
    return(0);
}

int btputmany(struct store *st, struct btnode *tree, struct btop *ops, int numops)
{
    return(btputmany2(st, tree, ops, numops, 0));
}

int btput(struct store *st, struct btnode *tree, block_t bl, void *buf, size_t len)
{
    struct btop ops[1];
    
    ops[0].blk = bl;
    ops[0].buf = buf;
    ops[0].len = len;
    return(btputmany(st, tree, ops, 1));
}

void btmkop(struct btop *op, block_t bl, void *buf, size_t len)
{
    memset(op, 0, sizeof(*op));
    op->blk = bl;
    op->buf = buf;
    op->len = len;
}

static int opcmp(const struct btop **op1, const struct btop **op2)
{
    if(ISDELOP(**op1) && ISDELOP(**op2))
	return((*op2)->blk - (*op1)->blk);
    else if(!ISDELOP(**op1) && ISDELOP(**op2))
	return(-1);
    else if(ISDELOP(**op1) && !ISDELOP(**op2))
	return(1);
    else
	return((*op1)->blk - (*op2)->blk);
}

void btsortops(struct btop *ops, int numops)
{
    qsort(ops, numops, sizeof(*ops), (int (*)(const void *, const void *))opcmp);
}

block_t btcount(struct store *st, struct btnode *tree)
{
    int d, f;
    struct btnode indir[BT_INDSZ];
    block_t c, ret;
    ssize_t sz;
    
    d = tree->d & 0x7f;
    f = tree->d & 0x80;
    
    if(f)
	return(1LL << (d * BT_INDBITS));
    
    if(d == 0)
	return(0);
    
    ret = 0;
    while(1) {
	if((sz = storeget(st, indir, BT_INDBSZ, &tree->a)) < 0)
	    return(-1);
	c = sz / sizeof(struct btnode);
	ret += (c - 1) * (1LL << ((d - 1) * BT_INDBITS));
	d = indir[c - 1].d & 0x7f;
	f = indir[c - 1].d & 0x80;
	if(f)
	    return(ret + (1LL << (d * BT_INDBITS)));
	tree = &indir[c - 1];
    }
}
