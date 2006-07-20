#define _LARGEFILE64_SOURCE
#define _XOPEN_SOURCE 500
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include "utils.h"

int readall(int fd, void *buf, size_t len, loff_t offset)
{
    int ret;
    
    while(len > 0) {
	/*
	if(lseek(fd, offset, SEEK_SET) != offset)
	    return(-1);
	ret = read(fd, buf, len);
	*/
	ret = pread64(fd, buf, len, offset);
	if(ret < 0)
	    return(-1);
	if(ret == 0) {
	    errno = ENODATA;
	    return(-1);
	}
	buf += ret;
	len -= ret;
	offset += ret;
    }
    return(0);
}

int writeall(int fd, const void *buf, size_t len, loff_t offset)
{
    int ret;
    
    while(len > 0) {
	/*
	if(lseek(fd, offset, SEEK_SET) != offset)
	    return(-1);
	ret = write(fd, buf, len);
	*/
	ret = pwrite64(fd, buf, len, offset);
	if(ret < 0)
	    return(-1);
	buf += ret;
	len -= ret;
	offset += ret;
    }
    return(0);
}
