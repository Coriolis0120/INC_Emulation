CC = gcc
MPICC = mpicc
CFLAGS = -Wall -O2
LDFLAGS = -lpcap -lpthread

all: soft_switch mpi_collective_test

soft_switch: soft_switch.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

mpi_collective_test: mpi_collective_test.c
	$(MPICC) $(CFLAGS) -o $@ $<

clean:
	rm -f soft_switch mpi_collective_test

.PHONY: all clean
