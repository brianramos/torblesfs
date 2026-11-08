CC ?= cc
CFLAGS ?= -O2
PREFIX ?= $(HOME)/.local
WARN = -Wall -Wno-unused-function -Wno-unused-variable -Wno-misleading-indentation
.PHONY: all native test bench install uninstall clean

all: native

# tfs: index, queries, maintenance, btrfs, content, dupes; tfsd: the FUSE mount with the watcher
CORE = native/tfscore.c native/tfsadmin.c
HDR = native/tfscore.h native/tfsadmin.h
TFS_SRC = native/tfs.c native/tfscontent.c native/tfsdupes.c native/tfsfiles.c
native: native/tfs native/tfsd
native/tfs: $(TFS_SRC) $(CORE) $(HDR) native/tfsext.h
	$(CC) $(CFLAGS) $(WARN) -o $@ $(TFS_SRC) $(CORE)
native/tfsd: native/tfsd.c $(CORE) $(HDR)
	$(CC) $(CFLAGS) $(WARN) -o $@ native/tfsd.c $(CORE)

test: native
	tests/run.sh

bench: native
	bench/bench.sh

install: native
	install -d $(PREFIX)/bin
	install -m 755 native/tfs native/tfsd $(PREFIX)/bin/

uninstall:
	rm -f $(PREFIX)/bin/tfs $(PREFIX)/bin/tfsd

clean:
	rm -f native/tfs native/tfsd
