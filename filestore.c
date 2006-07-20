#define _LARGEFILE64_SOURCE
#define _XOPEN_SOURCE 500
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <gcrypt.h>
#include <assert.h>

#include "utils.h"
#include "store.h"
#include "log.h"

#define LOGMAGIC "Dolda/Venti-1"
#define IDXMAGIC "Dolda/Index-1"
#define LOGENTMAGIC "\xca\xe5\x7a\x93"

typedef loff_t idx_t;

struct loghdr {
    char magic[sizeof(LOGMAGIC)];
};

struct idxhdr {
    char magic[sizeof(IDXMAGIC)];
    u_int64_t size;
};

struct idxent {
    struct addr addr;
    u_int64_t l, r;
    u_int64_t off;
};

struct logent {
    u_int8_t magic[4];
    struct addr name;
    u_int16_t len;
    u_int8_t fl;
};

struct fstore {
    int logfd;
    int idxfd;
    loff_t logsize;
    idx_t idxsize;
};

static int release(struct fstore *fst)
{
    if(fst->logfd >= 0) {
	fsync(fst->logfd);
	close(fst->logfd);
    }
    if(fst->idxfd >= 0) {
	fsync(fst->idxfd);
	close(fst->idxfd);
    }
    free(fst);
    return(0);
}

static int releaseg(struct store *st)
{
    return(release(st->pdata));
}

static void hash(const void *buf, size_t len, struct addr *a)
{
    gcry_md_hash_buffer(GCRY_MD_SHA256, a->hash, buf, len);
}

static int getidx(struct fstore *fst, idx_t i, struct idxent *ie)
{
    return(readall(fst->idxfd, ie, sizeof(*ie), sizeof(struct idxhdr) + i * sizeof(struct idxent)));
}

static int putidx(struct fstore *fst, idx_t i, struct idxent *ie)
{
    return(writeall(fst->idxfd, ie, sizeof(*ie), sizeof(struct idxhdr) + i * sizeof(struct idxent)));
}

static idx_t lookup(struct fstore *fst, struct addr *a, idx_t *parent)
{
    idx_t i;
    struct idxent ie;
    int c;
    
    if(fst->idxsize == 0) {
	if(parent != NULL)
	    *parent = -1;
	return(-1);
    }
    i = 0;
    while(1) {
	assert(!getidx(fst, i, &ie));
	c = addrcmp(a, &ie.addr);
	if(c < 0) {
	    if(ie.l == 0) {
		if(parent != NULL)
		    *parent = i;
		return(-1);
	    }
	    i = ie.l;
	} else if(c > 0) {
	    if(ie.r == 0) {
		if(parent != NULL)
		    *parent = i;
		return(-1);
	    }
	    i = ie.r;
	} else {
	    return(i);
	}
    }
}

static idx_t newindex(struct fstore *fst)
{
    size_t newsize;
    idx_t ni;
    struct idxent ne;
    struct idxhdr ih;
    
    /* XXX: Thread safety */
    ni = fst->idxsize++;
    newsize = sizeof(struct idxhdr) + fst->idxsize * sizeof(struct idxent);
    if(ftruncate(fst->idxfd, newsize))
	return(-1);
    ne.l = ne.r = 0;
    assert(!putidx(fst, ni, &ne));
    assert(!readall(fst->idxfd, &ih, sizeof(ih), 0));
    ih.size = fst->idxsize;
    assert(!writeall(fst->idxfd, &ih, sizeof(ih), 0));
    return(ni);
}

static int put(struct store *st, const void *buf, size_t len, struct addr *at)
{
    struct fstore *fst;
    struct addr pa;
    idx_t i, pi;
    struct idxent ie;
    loff_t leoff;
    int c;
    struct logent le;
    
    if(len > STORE_MAXBLSZ) {
	errno = E2BIG;
	return(-1);
    }

    fst = st->pdata;
    hash(buf, len, &pa);
    if(at != NULL)
	memcpy(at->hash, pa.hash, 32);
    
    if(lookup(fst, &pa, &pi) != -1)
	return(0);
    
    memcpy(le.magic, LOGENTMAGIC, 4);
    le.name = pa;
    le.len = len;
    le.fl = 0;
    /* XXX: Thread safety { */
    leoff = fst->logsize;
    fst->logsize += sizeof(le) + len;
    /* } */
    /* XXX: Handle data with embedded LOGENTMAGIC */
    writeall(fst->logfd, &le, sizeof(le), leoff);
    writeall(fst->logfd, buf, len, leoff + sizeof(le));

    i = newindex(fst);
    assert(!getidx(fst, i, &ie));
    ie.addr = pa;
    ie.off = leoff;
    assert(!putidx(fst, i, &ie));
    if(pi != -1) {
	assert(!getidx(fst, pi, &ie));
	c = addrcmp(&pa, &ie.addr);
	if(c < 0)
	    ie.l = i;
	else
	    ie.r = i;
	assert(!putidx(fst, pi, &ie));
    }
    
    return(0);
}

#define min(a, b) (((b) < (a))?(b):(a))

static ssize_t get(struct store *st, void *buf, size_t len, struct addr *at)
{
    idx_t i;
    struct idxent ie;
    struct fstore *fst;
    struct logent le;
    struct addr v;
    char tmpbuf[STORE_MAXBLSZ];
    
    fst = st->pdata;
    if((i = lookup(fst, at, NULL)) == -1) {
	errno = ENOENT;
	return(-1);
    }
    assert(!getidx(fst, i, &ie));
    
    if(readall(fst->logfd, &le, sizeof(le), ie.off)) {
	flog(LOG_CRIT, "could not read log entry: %s", strerror(errno));
	errno = EIO;
	return(-1);
    }
    if(memcmp(le.magic, LOGENTMAGIC, 4)) {
	flog(LOG_CRIT, "invalid magic in log");
	errno = EIO;
	return(-1);
    }
    if(addrcmp(&le.name, at)) {
	flog(LOG_CRIT, "did not receive correct block from log");
	errno = EIO;
	return(-1);
    }
    if(readall(fst->logfd, tmpbuf, le.len, ie.off + sizeof(le))) {
	flog(LOG_CRIT, "could not read log data: %s", strerror(errno));
	errno = EIO;
	return(-1);
    }
    hash(tmpbuf, le.len, &v);
    if(addrcmp(&v, &le.name)) {
	flog(LOG_CRIT, "log data did not verify against hash");
	errno = EIO;
	return(-1);
    }
    if(buf != NULL)
	memcpy(buf, tmpbuf, min(len, le.len));
    return(le.len);
}

static struct storeops fstops = {
    .release = releaseg,
    .put = put,
    .get = get,
};

struct store *newfstore(char *dir)
{
    struct store *st;
    struct fstore *fst;
    char tbuf[1024];
    struct loghdr lh;
    struct idxhdr ih;
    struct stat64 sb;
    
    fst = calloc(1, sizeof(*fst));
    fst->logfd = -1;
    fst->idxfd = -1;
    
    snprintf(tbuf, sizeof(tbuf), "%s/log", dir);
    if((fst->logfd = open(tbuf, O_RDWR | O_LARGEFILE)) < 0) {
	flog(LOG_ERR, "could not open log %s: %s", tbuf, strerror(errno));
	release(fst);
	return(NULL);
    }
    if(fstat64(fst->logfd, &sb)) {
	flog(LOG_ERR, "could not stat log: %s", strerror(errno));
	release(fst);
	return(NULL);
    }
    fst->logsize = sb.st_size;
    if(readall(fst->logfd, &lh, sizeof(lh), 0)) {
	flog(LOG_ERR, "could not read log header: %s", strerror(errno));
	release(fst);
	return(NULL);
    }
    if(memcmp(lh.magic, LOGMAGIC, sizeof(LOGMAGIC))) {
	flog(LOG_ERR, "invalid log magic");
	release(fst);
	return(NULL);
    }
    
    snprintf(tbuf, sizeof(tbuf), "%s/index", dir);
    if((fst->idxfd = open(tbuf, O_RDWR | O_LARGEFILE)) < 0) {
	flog(LOG_ERR, "could not open index %s: %s", tbuf, strerror(errno));
	release(fst);
	return(NULL);
    }
    if(fstat64(fst->idxfd, &sb)) {
	flog(LOG_ERR, "could not stat index: %s", strerror(errno));
	release(fst);
	return(NULL);
    }
    if(readall(fst->idxfd, &ih, sizeof(ih), 0)) {
	flog(LOG_ERR, "could not read index header: %s", strerror(errno));
	release(fst);
	return(NULL);
    }
    if(memcmp(ih.magic, IDXMAGIC, sizeof(IDXMAGIC))) {
	flog(LOG_ERR, "invalid index magic");
	release(fst);
	return(NULL);
    }
    if(sb.st_size != (sizeof(struct idxhdr) + ih.size * sizeof(struct idxent))) {
	flog(LOG_ERR, "invalid index size");
	release(fst);
	return(NULL);
    }
    fst->idxsize = ih.size;
    
    st = newstore(&fstops);
    st->pdata = fst;
    return(st);
}

int mkfstore(char *dir)
{
    char tbuf[1024];
    int fd;
    struct loghdr lh;
    struct idxhdr ih;
    
    if(access(dir, F_OK)) {
	if(mkdir(dir, 0700)) {
	    flog(LOG_ERR, "could not create %s: %s", dir, strerror(errno));
	    return(-1);
	}
    }
    
    snprintf(tbuf, sizeof(tbuf), "%s/log", dir);
    if((fd = open(tbuf, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0) {
	flog(LOG_ERR, "could not create log %s: %s", tbuf, strerror(errno));
	return(-1);
    }
    memcpy(lh.magic, LOGMAGIC, sizeof(LOGMAGIC));
    if(writeall(fd, &lh, sizeof(lh), 0)) {
	flog(LOG_ERR, "could not write log header: %s", strerror(errno));
	close(fd);
	return(-1);
    }
    close(fd);
    
    snprintf(tbuf, sizeof(tbuf), "%s/index", dir);
    if((fd = open(tbuf, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0) {
	flog(LOG_ERR, "could not create index %s: %s", tbuf, strerror(errno));
	return(-1);
    }
    memcpy(ih.magic, IDXMAGIC, sizeof(IDXMAGIC));
    ih.size = 0;
    if(writeall(fd, &ih, sizeof(ih), 0)) {
	flog(LOG_ERR, "could not write index header: %s", strerror(errno));
	close(fd);
	return(-1);
    }
    close(fd);
    return(0);
}
