BIN_DIR := bin
TARGET := $(BIN_DIR)/ctx

.PHONY: all clean demo

all: $(TARGET)

$(TARGET):
	mkdir -p $(BIN_DIR)
	printf '%s\n' '#!/usr/bin/env sh' > $(TARGET)
	printf '%s\n' 'set -eu' >> $(TARGET)
	printf '%s\n' 'ROOT_DIR=$$(CDPATH= cd -- "$$(dirname -- "$$0")/../../.." && pwd)' >> $(TARGET)
	printf '%s\n' 'cd "$$ROOT_DIR"' >> $(TARGET)
	printf '%s\n' 'exec ./bin/fxsh apps/ctx/src/main.fxsh "$$@"' >> $(TARGET)
	chmod +x $(TARGET)

demo: $(TARGET)
	$(TARGET) --dump 'a(b|c)*d' ad abcd accd abbd axd

clean:
	rm -f $(TARGET)
