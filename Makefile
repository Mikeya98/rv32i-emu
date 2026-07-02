CC       = gcc
CFLAGS   = -Wall -Wextra -std=c11 -O2 -g
INCDIR   = include
SRCDIR   = src
BUILDDIR = build
TARGET   = $(BUILDDIR)/rvemu

SRCS     = $(wildcard $(SRCDIR)/*.c)
OBJS     = $(SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)

.PHONY: all clean run test

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(INCDIR) -c -o $@ $<

run: $(TARGET)
	$(TARGET) $(BIN)

test: $(TARGET)
	@echo "=== RV32I Emulator Self-Test ==="
	$(TARGET) --selftest

clean:
	rm -rf $(BUILDDIR)
