#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "utils.h"
#include "store.h"
#include "blocktree.h"
#include "vcfs.h"

int main(int argc, char **argv)
{
    struct store *st;
    struct inode root;
    struct revrec frev;
    struct dentry dots;
    time_t now;
    int fd;
    char tbuf[1024];
    
    if(argc < 2) {
	fprintf(stderr, "usage; mkfs.vc DIR\n");
	exit(1);
    }
    if(mkfstore(argv[1]))
	exit(1);
    if((st = newfstore(argv[1])) == NULL)
	exit(1);
    
    now = time(NULL);
    
    memset(&dots, 0, sizeof(dots));
    dots.inode = 0;
    
    root.mode = S_IFDIR | 0755;
    root.mtime = root.ctime = now;
    root.size = 2;
    root.uid = getuid();
    root.gid = getgid();
    root.links = 2;
    root.data.d = 0;
    root.xattr.d = 0;
    strcpy(dots.name, ".");
    if(btput(st, &root.data, 0, &dots, sizeof(dots) - sizeof(dots.name) + 2)) {
	fprintf(stderr, "mkfs.vc: could not create root directory entries: %s\n", strerror(errno));
	exit(1);
    }
    strcpy(dots.name, "..");
    if(btput(st, &root.data, 1, &dots, sizeof(dots) - sizeof(dots.name) + 3)) {
	fprintf(stderr, "mkfs.vc: could not create root directory entries: %s\n", strerror(errno));
	exit(1);
    }
    
    frev.ct = now;
    frev.root.d = 0;
    if(btput(st, &frev.root, 0, &root, sizeof(root))) {
	fprintf(stderr, "mkfs.vc: could not store root directory inode: %s\n", strerror(errno));
	exit(1);
    }
    releasestore(st);
    
    snprintf(tbuf, sizeof(tbuf), "%s/revs", argv[1]);
    if((fd = open(tbuf, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0) {
	fprintf(stderr, "mkfs.vc: could not create revision database: %s\n", strerror(errno));
	exit(1);
    }
    if(writeall(fd, &frev, sizeof(frev), 0)) {
	fprintf(stderr, "mkfs.vc: could not write initial revision: %s\n", strerror(errno));
	exit(1);
    }
    fsync(fd);
    close(fd);
    return(0);
}
