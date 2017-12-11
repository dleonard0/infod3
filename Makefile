
CFLAGS += -ggdb -O0 -Wall
CPPFLAGS += -I.
#CPPFLAGS += -DSMALL              # disables text protocol and help

default: check infod/infod info/info

TESTS += t-proto
t-store: infod/t-store.o infod/store.o
	$(LINK.c) $(OUTPUT_OPTION) $^
TESTS += t-store
t-match: infod/t-match.o infod/match.o
	$(LINK.c) $(OUTPUT_OPTION) $^
TESTS += t-match
t-proto: lib/t-proto.o lib/proto.o lib/protofram.o lib/prototext.o \
	 lib/protobin.o lib/rxbuf.o
	$(LINK.c) $(OUTPUT_OPTION) $^
TESTS += t-server
t-server: infod/t-server.o infod/server.o
	$(LINK.c) $(OUTPUT_OPTION) $^
TESTS += t-list
t-list: infod/t-list.o
	$(LINK.c) $(OUTPUT_OPTION) $^
check: $(TESTS:%=%.checked)
%.checked: %
	$(RUNTEST) $(<D)/$(<F)
.PHONY: check %.checked


LIB_OBJS =  lib/proto.o
LIB_OBJS += lib/protofram.o
LIB_OBJS += lib/prototext.o
LIB_OBJS += lib/protobin.o
LIB_OBJS += lib/rxbuf.o
LIB_OBJS += lib/sockunix.o
LIB_OBJS += lib/socktcp.o
LIB_OBJS += lib/info.o
PICFLAGS = -fPIC
lib/libinfo3.a: lib/libinfo3.a($(LIB_OBJS))
lib/libinfo3.so: $(LIB_OBJS:.o=.po)
	$(LINK.c) $(OUTPUT_OPTION) -shared $^
%.po: %.c
	$(COMPILE.c) $(OUTPUT_OPTION) $(PICFLAGS) $<
LIBS = -Wl,-rpath,'$$ORIGIN/../lib' -Llib -linfo3


INFOD_OBJS =  infod/infod.o
INFOD_OBJS += infod/store.o
INFOD_OBJS += infod/match.o
INFOD_OBJS += infod/server.o
infod/infod: $(INFOD_OBJS) lib/libinfo3.so
	$(LINK.c) $(OUTPUT_OPTION) $(INFOD_OBJS) $(LIBS)


INFO_OBJS = info/info.o
info/info: $(INFO_OBJS) lib/libinfo3.so
	$(LINK.c) $(OUTPUT_OPTION) $(INFO_OBJS) $(LIBS)

clean:
	rm -f lib/*.o lib/*.po
	rm -f lib/libinfo3.a lib/libinfo3.so
	rm -f infod/infod infod/*.o
	rm -f info/info info/*.o
	rm -f $(TESTS)

INSTALL = install -D
prefix = /usr/local
bindir = $(prefix)/bin
sbindir = $(prefix)/sbin
libdir = $(prefix)/lib
incdir = $(prefix)/include
mandir = $(prefix)/man
install:
	$(INSTALL) -t $(DESTDIR)$(incdir) lib/info.h
	$(INSTALL) -s -t $(DESTDIR)$(libdir) lib/libinfo3.so
	$(INSTALL) -s -t $(DESTDIR)$(sbindir) infod/infod
	$(INSTALL) -s -t $(DESTDIR)$(bindir) info/info
	$(INSTALL) info/info.mdoc $(DESTDIR)$(mandir)/man1/info.1
	$(INSTALL) infod/infod.mdoc $(DESTDIR)$(mandir)/man8/infod.8
