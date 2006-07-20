#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include "store.h"

char buf[STORE_MAXBLSZ];

int main(int argc, char **argv)
{
    struct store *st;
    struct addr a;
    int ret;
    
    if(argc < 3) {
	fprintf(stderr, "usage: storeget DIR HASH\n");
	exit(1);
    }
    if((st = newfstore(argv[1])) == NULL)
	exit(1);
    parseaddr(argv[2], &a);
    if((ret = storeget(st, buf, STORE_MAXBLSZ, &a)) < 0) {
	perror(argv[2]);
	exit(1);
    }
    write(1, buf, ret);
    return(0);
}
