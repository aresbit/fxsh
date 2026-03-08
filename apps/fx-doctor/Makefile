APP := fx-doctor
BIN_DIR := bin
SCRIPT_SRC := scripts/$(APP)
LAUNCHER := $(BIN_DIR)/$(APP)
FXSH ?= ../../bin/fxsh
ENTRY := src/main.fxsh
REPO_ROOT := ../..

.PHONY: all launcher check check-native clean

all: launcher check check-native

$(BIN_DIR):
	@mkdir -p $@

launcher: $(LAUNCHER)

$(LAUNCHER): $(SCRIPT_SRC) | $(BIN_DIR)
	@cp $(SCRIPT_SRC) $(LAUNCHER)
	@chmod +x $(LAUNCHER)

check: launcher
	@FX_DOCTOR_MODE=help $(FXSH) $(ENTRY) >/dev/null

check-native: launcher
	@cd $(REPO_ROOT) && FX_DOCTOR_MODE=help FXD_ROOT=. FXD_KIND=repo FXD_ECOSYSTEMS=fxsh FXD_EXECUTED=false FXD_BRANCH=main FXD_GIT_STATE=clean FXD_READY=true FXD_PASSED=0 FXD_FAILED=0 FXD_SKIPPED=0 FXD_STEPS_TEXT='- none' FXD_ADVICE_TEXT='- none' FXD_STEP_JSON='' FXD_ADVICE_JSON='' FXD_ADVICE_COUNT=0 FXD_STEP_COUNT=0 FXD_GIT_DIRTY=false ./bin/fxsh --native-codegen apps/fx-doctor/$(ENTRY) >/dev/null

clean:
	rm -rf $(BIN_DIR)
