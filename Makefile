# jbi - just build it
PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
LIBDIR   ?= $(PREFIX)/lib
INCDIR   ?= $(PREFIX)/include
SHAREDIR ?= $(PREFIX)/share/jbi

CC     ?= cc
AR     ?= ar
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wextra -pedantic -fPIC -pthread -DJBI_SHAREDIR='"$(SHAREDIR)"'

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
SOEXT   = dylib
SOFLAGS = -dynamiclib
else
SOEXT   = so
SOFLAGS = -shared
endif

CONFIGS := $(wildcard configs/*.jbi)

.PHONY: all install uninstall clean check help

all: jbi libjbi.a libjbi.$(SOEXT)

jbi.o: jbi.c jbi.h
	$(CC) $(CFLAGS) -c -o $@ jbi.c

main.o: main.c jbi.h
	$(CC) $(CFLAGS) -c -o $@ main.c

libjbi.a: jbi.o
	$(AR) rcs $@ jbi.o

libjbi.$(SOEXT): jbi.o
	$(CC) $(SOFLAGS) -pthread -o $@ jbi.o

jbi: main.o libjbi.a
	$(CC) -pthread -o $@ main.o libjbi.a

install: all ## install bin, libs, header and configs under $(PREFIX)
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCDIR) $(DESTDIR)$(SHAREDIR)
	install -m 755 jbi $(DESTDIR)$(BINDIR)/jbi
	install -m 644 libjbi.a $(DESTDIR)$(LIBDIR)/libjbi.a
	install -m 755 libjbi.$(SOEXT) $(DESTDIR)$(LIBDIR)/libjbi.$(SOEXT)
	install -m 644 jbi.h $(DESTDIR)$(INCDIR)/jbi.h
	install -m 644 $(CONFIGS) $(DESTDIR)$(SHAREDIR)/
	@if [ -z "$(DESTDIR)" ] && [ "$(UNAME)" = Linux ]; then ldconfig 2>/dev/null || true; fi

uninstall: ## remove everything install put in place
	rm -f $(DESTDIR)$(BINDIR)/jbi
	rm -f $(DESTDIR)$(LIBDIR)/libjbi.a $(DESTDIR)$(LIBDIR)/libjbi.$(SOEXT)
	rm -f $(DESTDIR)$(INCDIR)/jbi.h
	for f in $(notdir $(CONFIGS)); do rm -f $(DESTDIR)$(SHAREDIR)/$$f; done
	-rmdir $(DESTDIR)$(SHAREDIR) 2>/dev/null
	@if [ -z "$(DESTDIR)" ] && [ "$(UNAME)" = Linux ]; then ldconfig 2>/dev/null || true; fi

check: all ## run the test suite
	./tests/test.sh

clean: ## remove build output
	rm -f *.o jbi libjbi.a libjbi.$(SOEXT)

help: ## show this help
	@echo "jbi - just build it"
	@echo
	@echo "  make            build jbi, libjbi.a and libjbi.$(SOEXT)"
	@echo "  make install    install to $(PREFIX)"
	@echo "                    bin      $(BINDIR)/jbi"
	@echo "                    lib      $(LIBDIR)/libjbi.a, libjbi.$(SOEXT)"
	@echo "                    header   $(INCDIR)/jbi.h"
	@echo "                    configs  $(SHAREDIR)/*.jbi"
	@echo "  make uninstall  remove all of the above"
	@echo "  make check      run tests"
	@echo "  make clean      remove build output"
	@echo "  make help       this text"
	@echo
	@echo "variables: PREFIX DESTDIR BINDIR LIBDIR INCDIR SHAREDIR CC CFLAGS"
