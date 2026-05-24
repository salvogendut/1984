CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -O2 -g $(shell sdl2-config --cflags)
LDFLAGS := $(shell sdl2-config --libs) -lm

SRCDIR  := src
BINDIR  := bin
TARGET  := $(BINDIR)/cpc1984

SRCS    := $(wildcard $(SRCDIR)/*.c)
OBJS    := $(SRCS:$(SRCDIR)/%.c=$(BINDIR)/%.o)

.PHONY: all clean

all: $(BINDIR) $(TARGET)

$(BINDIR):
	mkdir -p $(BINDIR)

$(TARGET): $(OBJS)
	$(CC) $^ $(LDFLAGS) -o $@

$(BINDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BINDIR)
