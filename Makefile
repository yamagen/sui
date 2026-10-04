CC      = cc
CFLAGS  = -O2 -Wall -Wextra

all: mkledger sui

mkledger: src/mkledger.c src/config.c src/config.h src/tiny-json.c src/tiny-json.h
	$(CC) $(CFLAGS) -o $@ src/mkledger.c src/config.c src/tiny-json.c

sui: src/sui.c src/tiny-json.c src/tiny-json.h
	$(CC) $(CFLAGS) -o $@ src/sui.c src/tiny-json.c

tiny-json: src/tiny-json.c src/tiny-json.h
	$(CC) $(CFLAGS) -DTJTEST -o $@ src/tiny-json.c

test: all
	sh ./tests/test-sui.sh

clean:
	rm -f mkledger sui tiny-json

.PHONY: all test clean

