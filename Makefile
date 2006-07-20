CFLAGS=-g -Wall

all: storeget storeput mkstore mkfs.vc vcfs

storeget: storeget.o store.o filestore.o log.o utils.o
	gcc $(CFLAGS) -o $@ $^ -lgcrypt

storeput: storeput.o store.o filestore.o log.o utils.o
	gcc $(CFLAGS) -o $@ $^ -lgcrypt

mkstore: mkstore.o store.o filestore.o log.o utils.o
	gcc $(CFLAGS) -o $@ $^ -lgcrypt

mkfs.vc: mkfs.vc.o store.o filestore.o log.o blocktree.o utils.o
	gcc $(CFLAGS) -o $@ $^ -lgcrypt

vcfs: vcfs.o store.o filestore.o log.o blocktree.o utils.o
	gcc $(CFLAGS) -o $@ $^ -lgcrypt -lfuse

vcfs.o: vcfs.c
	gcc -c $(CFLAGS) -o $@ $< -DFUSE_USE_VERSION=26 -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse

%.o: %.c
	gcc -c $(CFLAGS) -o $@ $<

clean:
	rm -f *.o storeget storeput mkstore mkfs.vc vcfs
