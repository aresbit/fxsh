APP := fx-superpowers
BIN_DIR := bin
SCRIPT_SRC := scripts/$(APP)
LAUNCHER := $(BIN_DIR)/$(APP)
FXSH ?= ../../bin/fxsh
ENTRY := src/main.fxsh

.PHONY: all launcher check clean

all: launcher check

$(BIN_DIR):
	@mkdir -p $@

launcher: $(LAUNCHER)

$(LAUNCHER): $(SCRIPT_SRC) | $(BIN_DIR)
	@cp $(SCRIPT_SRC) $(LAUNCHER)
	@chmod +x $(LAUNCHER)

check: launcher
	@FX_SP_MODE=help $(FXSH) $(ENTRY) >/dev/null
	@FX_SP_MODE=status $(FXSH) $(ENTRY) >/dev/null

clean:
	rm -rf $(BIN_DIR)

