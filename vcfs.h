#ifndef _VCFS_H
#define _VCFS_H

#include <time.h>
#include <inttypes.h>

#include "blocktree.h"

#define DIRBLSIZE 4
#define INOBLSIZE 4

typedef loff_t vc_ino_t;
typedef loff_t vc_rev_t;

struct revrec {
    u_int64_t ct;
    struct btnode root;
};

struct inode {
    u_int32_t mode;
    u_int64_t mtime, ctime;
    u_int64_t size;
    u_int32_t uid, gid;
    u_int32_t links;
    struct btnode data;
    struct btnode xattr;
};

struct dentry {
    u_int64_t inode;
    char name[256];
};

#endif
