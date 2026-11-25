CC ?= clang
CFLAGS := -Wall -Wextra -Werror -std=c11 -I../components/catprinter/include

.PHONY: all test clean
all: test_protocol

test_protocol: test_protocol.c ../components/catprinter/printer_protocol.c ../components/catprinter/include/printer_protocol.h
	$(CC) $(CFLAGS) -o $@ test_protocol.c ../components/catprinter/printer_protocol.c

test: test_protocol
	./test_protocol

clean:
	rm -f test_protocol
