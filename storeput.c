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
    int ret, o;
    
    if(argc < 3) {
	fprintf(stderr, "usage: storeget DIR\n");
	exit(1);
    }
    for(o = 0; (ret = read(0, buf + o, STORE_MAXBLSZ - o)) > 0; o += ret);
    if((st = newfstore(argv[1])) == NULL)
	exit(1);
    if((ret = storeput(st, buf, STORE_MAXBLSZ, &a)) < 0) {
	perror(argv[2]);
	exit(1);
    }
    printf("%s\n", formataddr(&a));
    return(0);
}
