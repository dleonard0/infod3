
TESTS += t-store
TESTS += t-match
CFLAGS += -ggdb -O0 -Wall

default: check

check: $(TESTS:%=%.tested)
%.tested: %
	$(<D)/$(<F)


t-store: t-store.o store.o
	$(LINK.c) -o $@ t-store.o store.o
t-match: t-match.o match.o
	$(LINK.c) -o $@ t-match.o match.o

clean:
	rm -f *.o
	rm -f t-store
	rm -f t-match
