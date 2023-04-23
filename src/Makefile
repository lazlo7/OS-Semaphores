CC=gcc
CFLAGS=-std=c11 -Wall -Wextra -pedantic
POSIX_SEM_FLAG=-pthread
POSIX_SHM_FLAG=-lrt

# Named Posix semaphores + Posix shared memory.
mark4: 4/prog.c
	$(CC) $(CFLAGS) $(POSIX_SEM_FLAG) $(POSIX_SHM_FLAG) 4/prog.c -o mark4 

# Unnamed Posix semaphores + Posix shared memory.
mark5: 5/prog.c
	$(CC) $(CFLAGS) $(POSIX_SEM_FLAG) $(POSIX_SHM_FLAG) 5/prog.c -o mark5

mark6: 6/prog.c
	$(CC) $(CFLAGS) $(POSIX_SEM_FLAG) $(POSIX_SHM_FLAG) 6/prog.c -o mark6

clean:
	rm -f mark4 mark5 mark6
