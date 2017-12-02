
TESTS += t-store
CFLAGS += -ggdb -O0 -Wall

default: check

check: $(TESTS:%=%.tested)
%.tested: %
	$(<D)/$(<F)


t-store: t-store.o store.o
	$(LINK.c) -o $@ t-store.o store.o

clean:
	rm -f *.o
	rm -f t-store
