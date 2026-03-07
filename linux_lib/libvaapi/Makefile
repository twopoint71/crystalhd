PREFIX ?= /usr
LIBDIR ?= $(PREFIX)/lib
DRIDIR ?= $(LIBDIR)/dri

CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -fPIC -fvisibility=hidden -D__LINUX_USER__
VA_CFLAGS ?= $(shell pkg-config --cflags libva 2>/dev/null)
VA_LIBS ?= $(shell pkg-config --libs libva 2>/dev/null)

INC = -I../libcrystalhd -I../../include -I../../include/link -I../../include/flea $(VA_CFLAGS)
LIBS = $(VA_LIBS)

TARGET = libva-crystalhd.so
SRC = va_crystalhd.cpp
OBJ = $(SRC:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) -shared -o $@ $^ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INC) -c -o $@ $<

install: $(TARGET)
	mkdir -p $(DESTDIR)$(DRIDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(DRIDIR)/$(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all install clean
