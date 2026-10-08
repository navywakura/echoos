# EchOS packages — build all tools into bin/
# Proprietary, Rogex Laboratories. C99 only; no external libraries.
CC      = cc
CFLAGS  = -std=c99 -O2 -Wall -Wextra
BIN     = bin
TOOLS   = $(BIN)/rg $(BIN)/fd $(BIN)/bat $(BIN)/eza $(BIN)/z \
          $(BIN)/unp $(BIN)/dust $(BIN)/duf $(BIN)/hyperfine \
          $(BIN)/localsend $(BIN)/rx-pkg $(BIN)/epk $(BIN)/echofetch

all: $(TOOLS)

$(BIN):
	mkdir -p $(BIN)

$(BIN)/rg: coretools/rg.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ coretools/rg.c

$(BIN)/fd: coretools/fd.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ coretools/fd.c

$(BIN)/bat: coretools/bat.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ coretools/bat.c

$(BIN)/eza: coretools/eza.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ coretools/eza.c

$(BIN)/z: coretools/z.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ coretools/z.c

$(BIN)/unp: coretools/unp.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ coretools/unp.c

$(BIN)/dust: coretools/dust.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ coretools/dust.c

$(BIN)/duf: coretools/duf.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ coretools/duf.c

$(BIN)/hyperfine: coretools/hyperfine.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ coretools/hyperfine.c

$(BIN)/localsend: coretools/localsend.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ coretools/localsend.c

$(BIN)/rx-pkg: rxpkg/rxpkg.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ rxpkg/rxpkg.c

$(BIN)/epk: rxpkg/rxpkg.c | $(BIN)
	$(CC) $(CFLAGS) -DEPK_CLI -o $@ rxpkg/rxpkg.c
	ln -sf epk $(BIN)/epk_add
	ln -sf epk $(BIN)/epk_delete

$(BIN)/echofetch: coretools/echofetch.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ coretools/echofetch.c

test: all
	sh tests/run_tests.sh

clean:
	rm -rf $(BIN)

.PHONY: all clean test
