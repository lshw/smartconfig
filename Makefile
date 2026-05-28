TARGET := smartconfig
SRC := src/smartconfig.c
OBJ := $(SRC:.c=.o)
DEP := $(OBJ:.o=.d)

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

CC ?= gcc
INSTALL ?= install

CPPFLAGS ?=
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
LDFLAGS ?=
LDLIBS ?=

.PHONY: all openwrt install uninstall clean distclean

all: $(TARGET)

openwrt: all

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

install: $(TARGET)
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET) $(OBJ) $(DEP)

distclean: clean

-include $(DEP)
