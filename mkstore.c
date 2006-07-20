#include <stdlib.h>
#include <stdio.h>

#include "store.h"

int main(int argc, char **argv)
{
    if(argc < 2) {
	fprintf(stderr, "usage: mkstore DIR\n");
	exit(1);
    }
    if(mkfstore(argv[1]))
	exit(1);
    return(0);
}
