CC=gcc
CFLAGS=-std=c11 -Wall -Wextra -pedantic
POSIX_SEM_FLAG=-pthread
POSIX_SHM_FLAG=-lrt

# Named Posix semaphores + Posix shared memory.
mark4: 4/prog.c
	$(CC) $(CFLAGS) $(POSIX_SEM_FLAG) $(POSIX_SHM_FLAG) 4/prog.c -o mark4 

clean:
	rm -f mark4
