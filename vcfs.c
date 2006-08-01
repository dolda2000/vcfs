#define _LARGEFILE64_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <fuse_lowlevel.h>

#include "utils.h"
#include "log.h"
#include "store.h"
#include "blocktree.h"
#include "vcfs.h"

/* XXX: The current i-numbering scheme sucks. */

struct btree {
    struct btree *l, *r;
    int h;
    void *d;
};

struct inoc {
    vc_ino_t inode;
    struct btnode inotab;
    fuse_ino_t cnum;
};

struct vcfsdata {
    struct store *st;
    int revfd;
    vc_rev_t currev;
    vc_ino_t nextino;
    struct btnode inotab;
    struct btree *inocbf, *inocbv;
    fuse_ino_t inocser;
};

#define max(a, b) (((b) > (a))?(b):(a))
static struct btnode nilnode = {0, };

static int btheight(struct btree *tree)
{
    if(tree == NULL)
	return(0);
    return(tree->h);
}

static void btsetheight(struct btree *tree)
{
    if(tree == NULL)
	return;
    tree->h = max(btheight(tree->l), btheight(tree->r)) + 1;
}

static void bbtrl(struct btree **tree);

static void bbtrr(struct btree **tree)
{
    struct btree *m, *l, *r;
    
    if(btheight((*tree)->l->r) > btheight((*tree)->l->l))
	bbtrl(&(*tree)->l);
    r = (*tree);
    l = r->l;
    m = l->r;
    r->l = m;
    btsetheight(r);
    l->r = r;
    btsetheight(l);
    *tree = l;
}

static void bbtrl(struct btree **tree)
{
    struct btree *m, *l, *r;
    
    if(btheight((*tree)->r->l) > btheight((*tree)->r->r))
	bbtrr(&(*tree)->r);
    l = (*tree);
    r = l->r;
    m = r->l;
    l->r = m;
    btsetheight(l);
    r->l = l;
    btsetheight(r);
    *tree = r;
}

static int bbtreeput(struct btree **tree, void *item, int (*cmp)(void *, void *))
{
    int c, r;
    
    if(*tree == NULL) {
	*tree = calloc(1, sizeof(**tree));
	(*tree)->d = item;
	(*tree)->h = 1;
	return(1);
    }
    if((c = cmp(item, (*tree)->d)) < 0)
	r = bbtreeput(&(*tree)->l, item, cmp);
    else if(c > 0)
	r = bbtreeput(&(*tree)->r, item, cmp);
    else
	return(0);
    btsetheight(*tree);
    if(btheight((*tree)->l) > btheight((*tree)->r) + 1)
	bbtrr(tree);
    if(btheight((*tree)->r) > btheight((*tree)->l) + 1)
	bbtrl(tree);
    return(r);
}

static void *btreeget(struct btree *tree, void *key, int (*cmp)(void *, void *))
{
    int c;
    
    while(1) {
	if(tree == NULL)
	    return(NULL);
	c = cmp(key, tree->d);
	if(c < 0)
	    tree = tree->l;
	else if(c > 0)
	    tree = tree->r;
	else
	    return(tree->d);
    }
}

static void dstrvcfs(struct vcfsdata *fsd)
{
    releasestore(fsd->st);
    fsync(fsd->revfd);
    close(fsd->revfd);
    free(fsd);
}

static int inoccmpbf(struct inoc *a, struct inoc *b)
{
    return(a->cnum - b->cnum);
}

static int inoccmpbv(struct inoc *a, struct inoc *b)
{
    if(a->inode < b->inode)
	return(-1);
    if(a->inode > b->inode)
	return(1);
    if(a->inotab.d < b->inotab.d)
	return(-1);
    if(a->inotab.d > b->inotab.d)
	return(1);
    return(addrcmp(&a->inotab.a, &b->inotab.a));
}

static struct inoc *getinocbf(struct vcfsdata *fsd, fuse_ino_t inode)
{
    struct inoc key;
    
    key.cnum = inode;
    return(btreeget(fsd->inocbf, &key, (int (*)(void *, void *))inoccmpbf));
}

static struct inoc *getinocbv(struct vcfsdata *fsd, vc_ino_t inode, struct btnode inotab)
{
    struct inoc key;
    
    key.inotab = inotab;
    key.inode = inode;
    return(btreeget(fsd->inocbv, &key, (int (*)(void *, void *))inoccmpbv));
}

static fuse_ino_t cacheinode(struct vcfsdata *fsd, vc_ino_t inode, struct btnode inotab)
{
    fuse_ino_t ret;
    struct inoc *inoc;
    
    if((inoc = getinocbv(fsd, inode, inotab)) != NULL)
	return(inoc->cnum);
    ret = fsd->inocser++;
    inoc = calloc(1, sizeof(*inoc));
    inoc->inode = inode;
    inoc->inotab = inotab;
    inoc->cnum = ret;
    bbtreeput(&fsd->inocbf, inoc, (int (*)(void *, void *))inoccmpbf);
    bbtreeput(&fsd->inocbv, inoc, (int (*)(void *, void *))inoccmpbv);
    return(ret);
}

static struct vcfsdata *initvcfs(char *dir)
{
    struct vcfsdata *fsd;
    char tbuf[1024];
    struct stat64 sb;
    struct revrec cr;
    
    fsd = calloc(1, sizeof(*fsd));
    snprintf(tbuf, sizeof(tbuf), "%s/revs", dir);
    if((fsd->revfd = open(tbuf, O_RDWR | O_LARGEFILE)) < 0) {
	flog(LOG_ERR, "could not open revision database: %s", strerror(errno));
	free(fsd);
	return(NULL);
    }
    if(fstat64(fsd->revfd, &sb)) {
	flog(LOG_ERR, "could not stat revision database: %s", strerror(errno));
	close(fsd->revfd);
	free(fsd);
	return(NULL);
    }
    if(sb.st_size % sizeof(struct revrec) != 0) {
	flog(LOG_ERR, "revision database has illegal size");
	close(fsd->revfd);
	free(fsd);
	return(NULL);
    }
    fsd->currev = (sb.st_size / sizeof(struct revrec)) - 1;
    assert(!readall(fsd->revfd, &cr, sizeof(cr), fsd->currev * sizeof(struct revrec)));
    fsd->inotab = cr.root;
    if((fsd->st = newfstore(dir)) == NULL) {
	close(fsd->revfd);
	free(fsd);
	return(NULL);
    }
    fsd->inocser = 1;
    cacheinode(fsd, 0, nilnode);
    if((fsd->nextino = btcount(fsd->st, &fsd->inotab)) < 0) {
	flog(LOG_ERR, "could not count inodes: %s");
	close(fsd->revfd);
	releasestore(fsd->st);
	free(fsd);
	return(NULL);
    }
    return(fsd);
}

static vc_ino_t dirlookup(struct vcfsdata *fsd, struct btnode *dirdata, const char *name, int *di)
{
    struct dentry dent;
    int i;
    ssize_t sz;
    
    for(i = 0; ; i++) {
	if((sz = btget(fsd->st, dirdata, i, &dent, sizeof(dent))) < 0) {
	    if(errno == ERANGE)
		errno = ENOENT;
	    return(-1);
	}
	if((dent.inode >= 0) && !strncmp(dent.name, name, sizeof(dent.name))) {
	    if(di != NULL)
		*di = i;
	    return(dent.inode);
	}
    }
}

static void fusedestroy(struct vcfsdata *fsd)
{
    dstrvcfs(fsd);
}

static void fillstat(struct stat *sb, struct inode *file)
{
    sb->st_mode = file->mode;
    sb->st_atime = (time_t)file->mtime;
    sb->st_mtime = (time_t)file->mtime;
    sb->st_ctime = (time_t)file->ctime;
    sb->st_size = file->size;
    sb->st_uid = file->uid;
    sb->st_gid = file->gid;
    sb->st_nlink = file->links;
}

static int getinode(struct vcfsdata *fsd, struct btnode inotab, vc_ino_t ino, struct inode *buf)
{
    ssize_t sz;
    
    if(inotab.d == 0)
	inotab = fsd->inotab;
    if((sz = btget(fsd->st, &inotab, ino, buf, sizeof(*buf))) < 0)
	return(-1);
    if(sz != sizeof(*buf)) {
	flog(LOG_ERR, "illegal size for inode %i", ino);
	errno = EIO;
	return(-1);
    }
    return(0);
}

static void fusegetattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct vcfsdata *fsd;
    struct stat sb;
    struct inoc *inoc;
    struct inode file;
    
    fsd = fuse_req_userdata(req);
    memset(&sb, 0, sizeof(sb));
    if((inoc = getinocbf(fsd, ino)) == NULL) {
	fuse_reply_err(req, ENOENT);
	return;
    }
    if(getinode(fsd, inoc->inotab, inoc->inode, &file)) {
	fuse_reply_err(req, errno);
	return;
    }
    fillstat(&sb, &file);
    fuse_reply_attr(req, &sb, 0);
}

static void fuselookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    struct vcfsdata *fsd;
    struct inode file;
    struct inoc *inoc;
    struct fuse_entry_param e;
    vc_ino_t target;
    
    fsd = fuse_req_userdata(req);
    if((inoc = getinocbf(fsd, parent)) == NULL) {
	fuse_reply_err(req, ENOENT);
	return;
    }
    if(getinode(fsd, inoc->inotab, inoc->inode, &file)) {
	fuse_reply_err(req, errno);
	return;
    }
    if((target = dirlookup(fsd, &file.data, name, NULL)) < 0) {
	fuse_reply_err(req, errno);
	return;
    }
    if(getinode(fsd, inoc->inotab, target, &file)) {
	fuse_reply_err(req, errno);
	return;
    }
    memset(&e, 0, sizeof(e));
    e.ino = cacheinode(fsd, target, inoc->inotab);
    fillstat(&e.attr, &file);
    fuse_reply_entry(req, &e);
}

static void fusereaddir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi)
{
    struct vcfsdata *fsd;
    struct inoc *inoc;
    struct inode file;
    struct dentry dent;
    struct stat sb;
    ssize_t sz, osz, bsz;
    char *buf;
    
    fsd = fuse_req_userdata(req);
    if((inoc = getinocbf(fsd, ino)) == NULL) {
	fuse_reply_err(req, ENOENT);
	return;
    }
    if(getinode(fsd, inoc->inotab, inoc->inode, &file)) {
	fuse_reply_err(req, errno);
	return;
    }
    bsz = 0;
    buf = NULL;
    while(bsz < size) {
	memset(&dent, 0, sizeof(dent));
	if((sz = btget(fsd->st, &file.data, off++, &dent, sizeof(dent))) < 0) {
	    if(errno == ERANGE) {
		if(buf != NULL)
		    break;
		fuse_reply_buf(req, NULL, 0);
		return;
	    }
	    fuse_reply_err(req, errno);
	    if(buf != NULL)
		free(buf);
	    return;
	}
	if(dent.inode < 0)
	    continue;
	osz = bsz;
	bsz += fuse_add_direntry(req, NULL, 0, dent.name, NULL, 0);
	if(bsz > size)
	    break;
	buf = realloc(buf, bsz);
	memset(&sb, 0, sizeof(sb));
	sb.st_ino = cacheinode(fsd, dent.inode, inoc->inotab);
	fuse_add_direntry(req, buf + osz, bsz - osz, dent.name, &sb, off);
    }
    fuse_reply_buf(req, buf, bsz);
    if(buf != NULL)
	free(buf);
}

static vc_rev_t commit(struct vcfsdata *fsd, struct btnode inotab)
{
    struct revrec rr;
    
    rr.ct = time(NULL);
    rr.root = inotab;
    if(writeall(fsd->revfd, &rr, sizeof(rr), (fsd->currev + 1) * sizeof(struct revrec))) {
	flog(LOG_CRIT, "could not write new revision: %s", strerror(errno));
	return(-1);
    }
    fsd->inotab = inotab;
    return(++fsd->currev);
}

static int deldentry(struct vcfsdata *fsd, struct inode *ino, int di)
{
    struct btop ops[2];
    struct dentry dent;
    ssize_t sz;
    
    if((di < 0) || (di >= ino->size)) {
	errno = ERANGE;
	return(-1);
    }
    if(di == ino->size - 1) {
	if(btput(fsd->st, &ino->data, ino->size - 1, NULL, 0))
	    return(-1);
    } else {
	if((sz = btget(fsd->st, &ino->data, ino->size - 1, &dent, sizeof(dent))) < 0)
	    return(-1);
	btmkop(ops + 0, di, &dent, sz);
	btmkop(ops + 1, ino->size - 1, NULL, 0);
	if(btputmany(fsd->st, &ino->data, ops, 2))
	    return(-1);
    }
    return(0);
}

static int setdentry(struct vcfsdata *fsd, struct inode *ino, int di, const char *name, vc_ino_t target)
{
    struct dentry dent;
    ssize_t sz;
    
    if(strlen(name) > 255) {
	errno = ENAMETOOLONG;
	return(-1);
    }
    memset(&dent, 0, sizeof(dent));
    strcpy(dent.name, name);
    dent.inode = target;
    sz = sizeof(dent) - sizeof(dent.name) + strlen(name) + 1;
    if((di == -1) || (di == ino->size)) {
	if(btput(fsd->st, &ino->data, ino->size, &dent, sz))
	    return(-1);
	ino->size++;
	return(0);
    }
    return(btput(fsd->st, &ino->data, di, &dent, sz));
}

static void fusemkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode)
{
    struct vcfsdata *fsd;
    struct inoc *inoc;
    struct inode file, new;
    struct btnode inotab;
    struct fuse_entry_param e;
    const struct fuse_ctx *ctx;
    struct btop ops[2];
    
    fsd = fuse_req_userdata(req);
    ctx = fuse_req_ctx(req);
    if((inoc = getinocbf(fsd, parent)) == NULL) {
	fuse_reply_err(req, ENOENT);
	return;
    }
    if(inoc->inotab.d != 0) {
	fuse_reply_err(req, EROFS);
	return;
    }
    if(getinode(fsd, inoc->inotab, inoc->inode, &file)) {
	fuse_reply_err(req, errno);
	return;
    }
    if(!S_ISDIR(file.mode)) {
	fuse_reply_err(req, ENOTDIR);
	return;
    }
    if(dirlookup(fsd, &file.data, name, NULL) != -1) {
	fuse_reply_err(req, EEXIST);
	return;
    }
    
    memset(&new, 0, sizeof(new));
    new.mode = S_IFDIR | mode;
    new.mtime = new.ctime = time(NULL);
    new.size = 0;
    new.uid = ctx->uid;
    new.gid = ctx->gid;
    new.links = 2;
    if(setdentry(fsd, &new, -1, ".", fsd->nextino) || setdentry(fsd, &new, -1, "..", inoc->inode)) {
	fuse_reply_err(req, errno);
	return;
    }
    
    inotab = fsd->inotab;
    if(setdentry(fsd, &file, -1, name, fsd->nextino)) {
	fuse_reply_err(req, errno);
	return;
    }
    file.links++;
    btmkop(ops + 0, inoc->inode, &file, sizeof(file));
    btmkop(ops + 1, fsd->nextino, &new, sizeof(new));
    if(btputmany(fsd->st, &inotab, ops, 2)) {
	fuse_reply_err(req, errno);
	return;
    }
    /*
    if(btput(fsd->st, &inotab, fsd->nextino, &new, sizeof(new))) {
	fuse_reply_err(req, errno);
	return;
    }
    if(btput(fsd->st, &inotab, inoc->inode, &file, sizeof(file))) {
	fuse_reply_err(req, errno);
	return;
    }
    */
    commit(fsd, inotab);
    
    memset(&e, 0, sizeof(e));
    e.ino = cacheinode(fsd, fsd->nextino++, nilnode);
    fillstat(&e.attr, &new);
    fuse_reply_entry(req, &e);
}

static void fuseunlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    struct vcfsdata *fsd;
    struct inoc *inoc;
    struct inode file;
    int di;
    struct btnode inotab;
    
    fsd = fuse_req_userdata(req);
    if((inoc = getinocbf(fsd, parent)) == NULL) {
	fuse_reply_err(req, ENOENT);
	return;
    }
    if(inoc->inotab.d != 0) {
	fuse_reply_err(req, EROFS);
	return;
    }
    if(getinode(fsd, inoc->inotab, inoc->inode, &file)) {
	fuse_reply_err(req, errno);
	return;
    }
    if(!S_ISDIR(file.mode)) {
	fuse_reply_err(req, ENOTDIR);
	return;
    }
    if(dirlookup(fsd, &file.data, name, &di) == -1) {
	fuse_reply_err(req, ENOENT);
	return;
    }
    inotab = fsd->inotab;
    if(deldentry(fsd, &file, di)) {
	fuse_reply_err(req, errno);
	return;
    }
    if(btput(fsd->st, &inotab, inoc->inode, &file, sizeof(file))) {
	fuse_reply_err(req, errno);
	return;
    }
    commit(fsd, inotab);
    fuse_reply_err(req, 0);
}

static struct fuse_lowlevel_ops fuseops = {
    .destroy = (void (*)(void *))fusedestroy,
    .lookup = fuselookup,
    .getattr = fusegetattr,
    .readdir = fusereaddir,
    .mkdir = fusemkdir,
    .rmdir = fuseunlink,
    .unlink = fuseunlink,
};

int main(int argc, char **argv)
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct fuse_session *fs;
    struct fuse_chan *ch;
    struct vcfsdata *fsd;
    char *mtpt;
    int err, fd;
    
    if((fsd = initvcfs(".")) == NULL)
	exit(1);
    if(fuse_parse_cmdline(&args, &mtpt, NULL, NULL) < 0)
	exit(1);
    if((fd = fuse_mount(mtpt, &args)) < 0)
	exit(1);
    if((fs = fuse_lowlevel_new(&args, &fuseops, sizeof(fuseops), fsd)) == NULL) {
	fuse_unmount(mtpt, fd);
	close(fd);
	fprintf(stderr, "vcfs: could not initialize fuse\n");
	exit(1);
    }
    fuse_set_signal_handlers(fs);
    if((ch = fuse_kern_chan_new(fd)) == NULL) {
	fuse_remove_signal_handlers(fs);
	fuse_unmount(mtpt, fd);
	fuse_session_destroy(fs);
	close(fd);
	exit(1);
    }
    
    fuse_session_add_chan(fs, ch);
    err = fuse_session_loop(fs);
    
    fuse_remove_signal_handlers(fs);
    fuse_unmount(mtpt, fd);
    fuse_session_destroy(fs);
    close(fd);
    return(err?1:0);
}
