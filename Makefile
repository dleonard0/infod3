
TESTS += t-store
TESTS += t-match
TESTS += t-proto
TESTS += t-server
CFLAGS += -ggdb -O0 -Wall

default: check

check: $(TESTS:%=%.tested)
%.tested: %
	$(<D)/$(<F)


t-store: t-store.o store.o
	$(LINK.c) -o $@ t-store.o store.o
t-match: t-match.o match.o
	$(LINK.c) -o $@ t-match.o match.o
t-proto: t-proto.o proto.o
	$(LINK.c) -o $@ t-proto.o proto.o
t-server: t-server.o server.o
	$(LINK.c) -o $@ t-server.o server.o

clean:
	rm -f *.o
	rm -f $(TESTS)
