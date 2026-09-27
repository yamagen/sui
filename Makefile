CC      = cc
CFLAGS  = -O2 -Wall -Wextra

all: mkledger sui

mkledger: src/mkledger.c
	$(CC) $(CFLAGS) -o $@ $<

sui: src/sui.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f mkledger sui

.PHONY: all clean
