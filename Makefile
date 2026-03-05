# fxsh - Functional Core Minimal Bash
# Modern C Makefile based on gnaro best practices

# Project Settings
debug ?= 0
NAME := fxsh
SRC_DIR := src
BUILD_DIR := build
INCLUDE_DIR := include
LIB_DIR := lib
TESTS_DIR := tests
BIN_DIR := bin

# Source files discovery (including subdirectories)
SRCS := $(wildcard $(SRC_DIR)/*.c) \
        $(wildcard $(SRC_DIR)/lexer/*.c) \
        $(wildcard $(SRC_DIR)/parser/*.c) \
        $(wildcard $(SRC_DIR)/types/*.c) \
        $(wildcard $(SRC_DIR)/comptime/*.c) \
        $(wildcard $(SRC_DIR)/codegen/*.c) \
        $(wildcard $(SRC_DIR)/interp/*.c) \
        $(wildcard $(SRC_DIR)/runtime/*.c) \
        $(wildcard $(SRC_DIR)/modules/*.c)

# Generate object file paths
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

# Compiler settings
CC := clang
LINTER := clang-tidy
FORMATTER := clang-format

# Compiler flags
# -std=gnu17: GNU C17 standard
# -D _GNU_SOURCE: Enable GNU extensions
# -D SP_IMPLEMENTATION: sp.h implementation in main.c only
# -Wall -Wextra: Strict warnings
# -Wno-strict-prototypes: Allow sp.h function declarations
# -pedantic: Pedantic warnings
CFLAGS := -std=gnu17 -D _GNU_SOURCE -Wall -Wextra -Wno-strict-prototypes -pedantic
CFLAGS += -I$(INCLUDE_DIR) -I$(LIB_DIR)
LDFLAGS := -lm

# Platform detection for Termux/Android
ifeq ($(shell uname -o),Android)
    CFLAGS += -DSP_PS_DISABLE
endif

# Debug/Release configuration
ifeq ($(debug), 1)
    CFLAGS := $(CFLAGS) -g -O0 -DDEBUG
else
    CFLAGS := $(CFLAGS) -Oz -DNDEBUG
endif

# Default target: build executable
.PHONY: all
default: $(NAME)

# Build executable
$(NAME): format dir $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BIN_DIR)/$@ $(OBJS)
	@echo "Built: $(BIN_DIR)/$@"

# Compile object files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# Generate compile_commands.json
.PHONY: bear
bear:
	bear -- make clean $(NAME)

# Run unit tests
.PHONY: test
test: dir
	@echo "Running unit tests..."
	@$(CC) $(CFLAGS) -o $(BIN_DIR)/$(NAME)_test \
		$(TESTS_DIR)/unit/*.c \
		$(filter-out $(BUILD_DIR)/main.o,$(OBJS)) \
		$(LDFLAGS)
	@$(BIN_DIR)/$(NAME)_test

# Run integration tests
.PHONY: test-integration
test-integration: $(NAME)
	@echo "Running integration tests..."
	@for f in $(TESTS_DIR)/integration/*.fxsh; do \
		if [ -f "$$f" ]; then \
			echo "Testing $$f..."; \
			$(BIN_DIR)/$(NAME) "$$f" || exit 1; \
		fi; \
	done

.PHONY: test-consistency
test-consistency: $(NAME)
	@echo "Running interpreter/native consistency smoke tests..."
	@sh $(TESTS_DIR)/integration/consistency.sh ./$(BIN_DIR)/$(NAME)

.PHONY: test-closure
test-closure: $(NAME)
	@echo "Running closure integration tests..."
	@sh $(TESTS_DIR)/integration/closure.sh ./$(BIN_DIR)/$(NAME)

.PHONY: test-closure-native
test-closure-native: $(NAME)
	@echo "Running native closure consistency tests..."
	@sh $(TESTS_DIR)/integration/closure_native.sh ./$(BIN_DIR)/$(NAME)

.PHONY: test-native-codegen
test-native-codegen: $(NAME)
	@echo "Running native-codegen smoke tests..."
	@sh $(TESTS_DIR)/integration/native_codegen.sh ./$(BIN_DIR)/$(NAME)

.PHONY: test-type-annotations
test-type-annotations: $(NAME)
	@echo "Running type annotation integration tests..."
	@sh $(TESTS_DIR)/integration/type_annotations.sh ./$(BIN_DIR)/$(NAME)

# Run linter
.PHONY: lint
lint:
	$(LINTER) --config-file=.clang-tidy \
		$(SRC_DIR)/*/*.c $(SRC_DIR)/*.c \
		$(INCLUDE_DIR)/*.h \
		-- $(CFLAGS) 2>/dev/null || true

# Format code
.PHONY: format
format:
	$(FORMATTER) -style=file -i \
		$(SRC_DIR)/*/*.c $(SRC_DIR)/*.c \
		$(INCLUDE_DIR)/*.h \
		$(TESTS_DIR)/unit/*.c 2>/dev/null || true

# Run memory checks
.PHONY: check
check: $(NAME)
	valgrind -s --leak-check=full --show-leak-kinds=all \
		$(BIN_DIR)/$(NAME) --help 2>/dev/null || true

# Setup development environment (Termux/Ubuntu)
.PHONY: setup
setup:
	@echo "Setting up development environment..."
	@pkg update -y && pkg upgrade -y || apt update -y
	@pkg install -y clang clang-tidy clang-format valgrind bear make git \
		|| apt install -y clang clang-tidy clang-format valgrind bear make git
	@echo "Dependencies installed."

# Create build directories
.PHONY: dir
dir:
	@mkdir -p $(BUILD_DIR)/{lexer,parser,types,comptime,codegen,modules}
	@mkdir -p $(BUILD_DIR)/interp
	@mkdir -p $(BUILD_DIR)/runtime
	@mkdir -p $(BIN_DIR)

# Clean build artifacts
.PHONY: clean
clean:
	@rm -rf $(BUILD_DIR) $(BIN_DIR)
	@echo "Cleaned build artifacts"

# Install fxsh to system
.PHONY: install
install: $(NAME)
	@cp $(BIN_DIR)/$(NAME) $(PREFIX)/bin/$(NAME) 2>/dev/null || \
		cp $(BIN_DIR)/$(NAME) /usr/local/bin/$(NAME)
	@echo "Installed $(NAME) to $$(which $(NAME))"

# Show help
.PHONY: help
help:
	@echo "fxsh - Functional Core Minimal Bash"
	@echo ""
	@echo "Targets:"
	@echo "  make              - Build fxsh compiler"
	@echo "  make debug=1      - Build with debug symbols"
	@echo "  make test         - Run unit tests"
	@echo "  make test-integration - Run integration tests"
	@echo "  make test-consistency - Run interpreter/native consistency smoke tests"
	@echo "  make test-closure - Run closure integration tests (interpreter)"
	@echo "  make test-closure-native - Run closure consistency tests in native mode"
	@echo "  make test-native-codegen - Run native-codegen smoke tests"
	@echo "  make test-type-annotations - Run type annotation integration tests"
	@echo "  make lint         - Run static analysis"
	@echo "  make format       - Format code"
	@echo "  make check        - Run memory checks"
	@echo "  make setup        - Install dependencies"
	@echo "  make clean        - Clean build artifacts"
	@echo "  make bear         - Generate compile_commands.json"
	@echo "  make install      - Install to system"
