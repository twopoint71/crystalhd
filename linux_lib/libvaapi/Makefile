PREFIX ?= /usr
LIBDIR ?= $(PREFIX)/lib
DRIDIR ?= $(LIBDIR)/dri

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -fPIC -fvisibility=hidden
VA_CFLAGS ?= $(shell pkg-config --cflags libva 2>/dev/null)
VA_LIBS ?= $(shell pkg-config --libs libva 2>/dev/null)

INC = -I../libcrystalhd -I../../include $(VA_CFLAGS)
LIBS = -L../libcrystalhd -lcrystalhd $(VA_LIBS)

TARGET = libva-crystalhd.so
SRC = va_crystalhd.c
OBJ = $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -shared -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

install: $(TARGET)
	mkdir -p $(DESTDIR)$(DRIDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(DRIDIR)/$(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all install clean
