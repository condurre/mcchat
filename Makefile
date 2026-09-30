CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2 -std=c11
LDFLAGS ?=
LDLIBS  := -lpthread

BIN := mcchat
SRC := src/mcchat.c

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f $(BIN)
