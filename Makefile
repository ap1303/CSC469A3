memlib: memlib.c memlib.h
	gcc -Wall -g -c -o memlib.o memlib.c

mm_thread: mm_thread.c mm_thread.h
	gcc -Wall -g -c -o mm_thread.o mm_thread.c

a3alloc: a3alloc.c mm_thread.o memlib.o
	gcc -std=c11 -march='native' -latomic -Wall -g -c -o a3alloc.o a3alloc.c
	gcc -std=c11 -march='native' -latomic -Wall -g -o a3alloc a3alloc.o mm_thread.o memlib.o
