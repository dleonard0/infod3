
TESTS += t-store
TESTS += t-match
TESTS += t-proto
TESTS += t-server
CFLAGS += -ggdb -O0 -Wall

# The SMALL flag disables buffered protocols, leaving
# only PROTO_MODE_FRAMED available.
#CPPFLAGS += -DSMALL

default: check infod

check: $(TESTS:%=%.tested)
%.tested: %
	$(<D)/$(<F)

PROTO_OBJS = proto.o protofram.o prototext.o protobin.o rxbuf.o
INFOD_OBJS = infod.o store.o match.o server.o
INFOD_OBJS += $(PROTO_OBJS)

infod: $(INFOD_OBJS)
	$(LINK.c) -o $@ $(INFOD_OBJS)

t-store: t-store.o store.o
	$(LINK.c) -o $@ t-store.o store.o
t-match: t-match.o match.o
	$(LINK.c) -o $@ t-match.o match.o
t-proto: t-proto.o $(PROTO_OBJS)
	$(LINK.c) -o $@ t-proto.o $(PROTO_OBJS)
t-server: t-server.o server.o
	$(LINK.c) -o $@ t-server.o server.o

clean:
	rm -f *.o
	rm -f $(TESTS)
	rm -f infod
