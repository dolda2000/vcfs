#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "store.h"

struct store *newstore(struct storeops *ops)
{
    struct store *new;
    
    new = malloc(sizeof(*new));
    new->ops = ops;
    new->pdata = NULL;
    new->cache = calloc(4096 * 4, sizeof(struct storecache));
    return(new);
}

#define min(a, b) (((b) < (a))?(b):(a))

static ssize_t cacheget(struct store *st, struct addr *a, void *buf, size_t len)
{
    int he, i;

    he = a->hash[0] | ((a->hash[1] & 0x0f) << 8);
    for(i = 0; i < 4; i++) {
	if(!addrcmp(&st->cache[he * 4 + i].a, a))
	    break;
    }
    if(i == 4)
	return(-2);
    if(st->cache[he * 4 + i].data != NULL)
	memcpy(buf, st->cache[he * 4 + i].data, min(len, st->cache[he * 4 + i].dlen));
    return(st->cache[he * 4 + i].dlen);
}

static void cacheput(struct store *st, struct addr *a, const void *data, ssize_t len)
{
    int he, i;
    struct storecache tmp;
    
    he = a->hash[0] | ((a->hash[1] & 0x0f) << 8);
    for(i = 0; i < 4; i++) {
	if(!addrcmp(&st->cache[he * 4 + i].a, a))
	    break;
    }
    if(i == 0)
	return;
    if(i < 4) {
	tmp = st->cache[he * 4 + i];
	memmove(&st->cache[he * 4 + 1], &st->cache[he * 4], i * sizeof(struct storecache));
	st->cache[he * 4] = tmp;
	return;
    }
    if(st->cache[he * 4 + 3].data != NULL)
	free(st->cache[he * 4 + 3].data);
    memmove(&st->cache[he * 4 + 1], &st->cache[he * 4], 3 * sizeof(struct storecache));
    st->cache[he * 4].a = *a;
    if(len > 0)
	st->cache[he * 4].data = memcpy(malloc(len), data, len);
    else
	st->cache[he * 4].data = NULL;
    st->cache[he * 4].dlen = len;
}

int storeput(struct store *st, const void *buf, size_t len, struct addr *at)
{
    int ret;
    struct addr na;

    ret = st->ops->put(st, buf, len, &na);
    if(!ret)
	cacheput(st, &na, buf, len);
    if(at != NULL)
	*at = na;
    return(ret);
}

ssize_t storeget(struct store *st, void *buf, size_t len, struct addr *at)
{
    ssize_t sz;
    struct addr at2;
    
    at2 = *at;
    sz = cacheget(st, at2, buf, len);
    if(sz != -2) {
	if(sz == -1)
	    errno = ENOENT;
	return(sz);
    }
    sz = st->ops->get(st, buf, len, at2);
    if((sz < 0) && (errno == ENOENT))
	cacheput(st, at2, NULL, -1);
    else if(sz >= 0)
	cacheput(st, at2, buf, sz);
    return(sz);
}

int releasestore(struct store *st)
{
    int err;
    
    if((err = st->ops->release(st)) != 0)
	return(err);
    free(st);
    return(0);
}

int addrcmp(struct addr *a1, struct addr *a2)
{
    return(memcmp(a1->hash, a2->hash, 32));
}

char *formataddr(struct addr *a)
{
    int i;
    static char buf[65];
    
    for(i = 0; i < 32; i++)
	sprintf(buf + (i * 2), "%02x", a->hash[i]);
    buf[64] = 0;
    return(buf);
}

static int hex2int(char hex)
{
    if((hex >= 'a') && (hex <= 'f'))
	return(hex - 'a' + 10);
    if((hex >= 'A') && (hex <= 'F'))
	return(hex - 'A' + 10);
    if((hex >= '0') && (hex <= '9'))
	return(hex - '0');
    return(-1);
}

int parseaddr(char *p, struct addr *a)
{
    int i, d;
    
    for(i = 0; i < 32; i++) {
	if((d = hex2int(*p++)) < 0)
	    return(-1);
	while(*p == ' ')
	    p++;
	a->hash[i] = d << 4;
	if((d = hex2int(*p++)) < 0)
	    return(-1);
	while(*p == ' ')
	    p++;
	a->hash[i] |= d;
    }
    if(*p != 0)
	return(-1);
    return(0);
}

int niladdr(struct addr *a)
{
    int i;
    
    for(i = 0; i < 32; i++) {
	if(a->hash[i])
	    return(0);
    }
    return(1);
}
