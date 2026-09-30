CC      = gcc
CFLAGS  = -Wall -Wextra -std=c17
LDLIBS  = -lpthread

planificador: planificador.c
	$(CC) $(CFLAGS) planificador.c -o planificador $(LDLIBS)

test: planificador
	./planificador plan.txt 3

clean:
	rm -f planificador
