#ifndef _UTILS_H
#define _UTILS_H

int readall(int fd, void *buf, size_t len, loff_t offset);
int writeall(int fd, const void *buf, size_t len, loff_t offset);

#endif
