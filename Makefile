
# The default file that infod will use for storage
STORE_PATH = /run/infod.store


CFLAGS += -ggdb -Os -pedantic -Wall
CPPFLAGS += -I.
#CPPFLAGS += -DSMALL              # disables text protocol and help
ARFLAGS = rvU

default: all check

all: info infod libinfo3.a libinfo3.so

# Allow building from outside source directory
SRCDIR ?= .
CPPFLAGS += -I$(SRCDIR)
VPATH = $(SRCDIR):.

TESTS += t-store
TESTS += t-match
TESTS += t-proto
TESTS += t-server
TESTS += t-list
TESTS += t-lib-info
TESTS += t-info
t-store: daemon-t-store.o daemon-store.o
	$(LINK.c) $(OUTPUT_OPTION) $^
t-match: daemon-t-match.o daemon-match.o
	$(LINK.c) $(OUTPUT_OPTION) $^
t-proto: lib-t-proto.o lib-proto.o lib-protofram.o lib-prototext.o \
	 lib-protobin.o lib-rxbuf.o
	$(LINK.c) $(OUTPUT_OPTION) $^
t-server: daemon-t-server.o daemon-server.o
	$(LINK.c) $(OUTPUT_OPTION) $^
t-list: daemon-t-list.o
	$(LINK.c) $(OUTPUT_OPTION) $^
t-lib-info: lib-t-info.o lib-info.o
	$(LINK.c) $(OUTPUT_OPTION) $^
t-info: $(SRCDIR)/t-info.sh info infod
	install -m 755 $(SRCDIR)/t-info.sh $@
check: $(TESTS:%=%.checked)
%.checked: %
	$(RUNTEST) $(<D)/$(<F)
.PHONY: check %.checked


LIB_OBJS =  lib-proto.o
LIB_OBJS += lib-protofram.o
LIB_OBJS += lib-prototext.o
LIB_OBJS += lib-protobin.o
LIB_OBJS += lib-rxbuf.o
LIB_OBJS += lib-sockunix.o
LIB_OBJS += lib-socktcp.o
LIB_OBJS += lib-info.o
PICFLAGS = -fPIC
libinfo3.a: libinfo3.a($(LIB_OBJS))
libinfo3.so: $(LIB_OBJS:.o=.po)
	$(LINK.c) $(OUTPUT_OPTION) -shared $^
LIBS = -Wl,-rpath,'$$ORIGIN' -L. -linfo3

INFOD_OBJS =  daemon-infod.o
INFOD_OBJS += daemon-store.o
INFOD_OBJS += daemon-match.o
INFOD_OBJS += daemon-server.o
infod: $(INFOD_OBJS) libinfo3.so
	$(LINK.c) $(OUTPUT_OPTION) $(INFOD_OBJS) $(LIBS)

daemon-infod.o: daemon/infod.c storepath.h
	$(COMPILE.c) $(OUTPUT_OPTION) $<
storepath.h: Makefile
	printf '#define STORE_PATH "%s"\n' "$(STORE_PATH)" >$@

INFO_OBJS = client-info.o
info: $(INFO_OBJS) libinfo3.so
	$(LINK.c) $(OUTPUT_OPTION) $(INFO_OBJS) $(LIBS)

client-%.o: client/%.c;	$(COMPILE.c) $(OUTPUT_OPTION) $<
lib-%.o: lib/%.c;	$(COMPILE.c) $(OUTPUT_OPTION) $<
daemon-%.o: daemon/%.c;	$(COMPILE.c) $(OUTPUT_OPTION) $<
lib-%.po: lib/%.c;	$(COMPILE.c) $(OUTPUT_OPTION) $(PICFLAGS) $<

clean:
	rm -f *.o *.po
	rm -f libinfo3.a libinfo3.so
	rm -f infod info
	rm -f $(TESTS)
	rm -f storepath.h

PREFIX = /usr/local
bindir = $(PREFIX)/bin
sbindir = $(PREFIX)/sbin
libdir = $(PREFIX)/lib
incdir = $(PREFIX)/include
mandir = $(PREFIX)/man

INSTALL = install -D
INSTALL_DATA = $(INSTALL) -m 644
INSTALL_BIN  = $(INSTALL) -m 755

install:
	$(INSTALL_DATA) -t $(DESTDIR)$(incdir) $(SRCDIR)/lib/info.h
	$(INSTALL_DATA) -s -t $(DESTDIR)$(libdir) libinfo3.so
	-$(INSTALL_DATA) -s -t $(DESTDIR)$(libdir) libinfo3.a
	$(INSTALL_BIN) -s -t $(DESTDIR)$(sbindir) infod
	$(INSTALL_BIN) -s -t $(DESTDIR)$(bindir) info
	$(INSTALL_DATA) $(SRCDIR)/client/info.mdoc \
				$(DESTDIR)$(mandir)/man1/info.1
	$(INSTALL_DATA) $(SRCDIR)/daemon/infod.mdoc \
				$(DESTDIR)$(mandir)/man8/infod.8
	$(INSTALL_DATA) $(SRCDIR)/lib/libinfo.mdoc \
				$(DESTDIR)$(mandir)/man3/libinfo.3

%.md: %.mdoc
	man -Tutf8 $< | sed \
	  -e 's,_\(.\),i\1/i,g' \
	  -e 's,\(.\)\1,b\1/b,g' \
	  -e 's,/\([ib]\)\1,,g' \
	  -e 's,&,\&amp;,g' \
	  -e 's,<,\&lt;,g' \
	  -e 's,>,\&gt;,g' \
	  -e 's,/\([bi]\),</\1>,g' \
	  -e 's,\([bi]\),<\1>,g' \
	  -e '1s,^,<pre>,' \
	  -e '$$s,$$,</pre>,' \
	  >$@
