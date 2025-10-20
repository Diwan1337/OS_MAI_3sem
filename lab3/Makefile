CC=gcc
CFLAGS=-Wall -Wextra -O2

all: parent_shm child_shm

parent_shm: parent_shm.c
	$(CC) $(CFLAGS) parent_shm.c -o parent_shm -pthread

child_shm: child_shm.c
	$(CC) $(CFLAGS) child_shm.c -o child_shm -pthread

clean:
	rm -f parent_shm child_shm
