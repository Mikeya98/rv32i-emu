CC       = gcc
CFLAGS   = -Wall -Wextra -std=c11 -O2 -g
INCDIR   = include
SRCDIR   = src
BUILDDIR = build
TARGET   = $(BUILDDIR)/rvemu

SRCS     = $(wildcard $(SRCDIR)/*.c)
OBJS     = $(SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)

.PHONY: all clean run test demo debug-demo

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

# 生成一个 demo ELF (Python 手写汇编器, 不需要 RISC-V 工具链) 并运行
demo: $(TARGET)
	@mkdir -p $(BUILDDIR)
	python tests/gen_demo_elf.py $(BUILDDIR)/demo.elf
	$(TARGET) $(BUILDDIR)/demo.elf

# 在调试器里打开同一个 demo
debug-demo: $(TARGET)
	@mkdir -p $(BUILDDIR)
	python tests/gen_demo_elf.py $(BUILDDIR)/demo.elf
	$(TARGET) -d $(BUILDDIR)/demo.elf

clean:
	rm -rf $(BUILDDIR)
