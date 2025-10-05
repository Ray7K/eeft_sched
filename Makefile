# -----------------------------
# Project configuration
# -----------------------------
PACKAGE_NAME = eeft_sched
CC = clang
CFLAGS = -Wall -Wextra -std=c11 -Iinclude -pthread

SRC_DIR = src
TEST_DIR = tests
TARGET_DIR = target

DEBUG_DIR = $(TARGET_DIR)/debug
RELEASE_DIR = $(TARGET_DIR)/release

DEBUG_OBJ_DIR = $(DEBUG_DIR)/obj
RELEASE_OBJ_DIR = $(RELEASE_DIR)/obj
DEBUG_BIN_DIR = $(DEBUG_DIR)/bin
RELEASE_BIN_DIR = $(RELEASE_DIR)/bin

# -----------------------------
# Source and test files
# -----------------------------
SRC_FILES = $(wildcard $(SRC_DIR)/*.c)
TEST_FILES = $(wildcard $(TEST_DIR)/*.c)

# -----------------------------
# Object file paths
# -----------------------------
DEBUG_OBJ_FILES = $(patsubst $(SRC_DIR)/%.c,$(DEBUG_OBJ_DIR)/%.o,$(SRC_FILES))
RELEASE_OBJ_FILES = $(patsubst $(SRC_DIR)/%.c,$(RELEASE_OBJ_DIR)/%.o,$(SRC_FILES))

DEBUG_MAIN_OBJ = $(DEBUG_OBJ_DIR)/main.o
RELEASE_MAIN_OBJ = $(RELEASE_OBJ_DIR)/main.o

DEBUG_OTHER_OBJS = $(filter-out $(DEBUG_MAIN_OBJ), $(DEBUG_OBJ_FILES))
RELEASE_OTHER_OBJS = $(filter-out $(RELEASE_MAIN_OBJ), $(RELEASE_OBJ_FILES))

DEBUG_TEST_OBJ_FILES = $(patsubst $(TEST_DIR)/%.c,$(DEBUG_OBJ_DIR)/tests/%.o,$(TEST_FILES))
RELEASE_TEST_OBJ_FILES = $(patsubst $(TEST_DIR)/%.c,$(RELEASE_OBJ_DIR)/tests/%.o,$(TEST_FILES))

# -----------------------------
# Default targets
# -----------------------------
all: release debug

debug: $(DEBUG_BIN_DIR)/$(PACKAGE_NAME) $(DEBUG_BIN_DIR)/$(PACKAGE_NAME)-tests
release: $(RELEASE_BIN_DIR)/$(PACKAGE_NAME) $(RELEASE_BIN_DIR)/$(PACKAGE_NAME)-tests

# -----------------------------
# Run targets
# -----------------------------
run: $(DEBUG_BIN_DIR)/$(PACKAGE_NAME)
	$(DEBUG_BIN_DIR)/$(PACKAGE_NAME) $(ARGS)

run-release: $(RELEASE_BIN_DIR)/$(PACKAGE_NAME)
	$(RELEASE_BIN_DIR)/$(PACKAGE_NAME) $(ARGS)

test: $(DEBUG_BIN_DIR)/$(PACKAGE_NAME)-tests
	$(DEBUG_BIN_DIR)/$(PACKAGE_NAME)-tests $(ARGS)

test-release: $(RELEASE_BIN_DIR)/$(PACKAGE_NAME)-tests
	$(RELEASE_BIN_DIR)/$(PACKAGE_NAME)-tests $(ARGS)

# -----------------------------
# Clean
# -----------------------------
clean:
	rm -rf $(DEBUG_DIR)
	rm -rf $(RELEASE_DIR)

# -----------------------------
# Build main application
# -----------------------------
$(DEBUG_BIN_DIR)/$(PACKAGE_NAME): $(DEBUG_OBJ_FILES) | $(DEBUG_BIN_DIR)
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $^ -o $@

$(RELEASE_BIN_DIR)/$(PACKAGE_NAME): $(RELEASE_OBJ_FILES) | $(RELEASE_BIN_DIR)
	$(CC) $(CFLAGS) -O3 $^ -o $@

# -----------------------------
# Build test application (exclude main.o)
# -----------------------------
$(DEBUG_BIN_DIR)/$(PACKAGE_NAME)-tests: $(DEBUG_OTHER_OBJS) $(DEBUG_TEST_OBJ_FILES) | $(DEBUG_BIN_DIR)
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined $^ -o $@

$(RELEASE_BIN_DIR)/$(PACKAGE_NAME)-tests: $(RELEASE_OTHER_OBJS) $(RELEASE_TEST_OBJ_FILES) | $(RELEASE_BIN_DIR)
	$(CC) $(CFLAGS) -O3 $^ -o $@

# -----------------------------
# Compile main source objects
# -----------------------------
$(DEBUG_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(DEBUG_OBJ_DIR)
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined -c $< -o $@

$(RELEASE_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(RELEASE_OBJ_DIR)
	$(CC) $(CFLAGS) -O3 -c $< -o $@

# -----------------------------
# Compile test source objects
# -----------------------------
$(DEBUG_OBJ_DIR)/tests/%.o: $(TEST_DIR)/%.c | $(DEBUG_OBJ_DIR)/tests
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined -c $< -o $@

$(RELEASE_OBJ_DIR)/tests/%.o: $(TEST_DIR)/%.c | $(RELEASE_OBJ_DIR)/tests
	$(CC) $(CFLAGS) -O3 -c $< -o $@

# -----------------------------
# Ensure directories exist
# -----------------------------
$(DEBUG_OBJ_DIR):
	mkdir -p $(DEBUG_OBJ_DIR)

$(RELEASE_OBJ_DIR):
	mkdir -p $(RELEASE_OBJ_DIR)

$(DEBUG_BIN_DIR):
	mkdir -p $(DEBUG_BIN_DIR)

$(RELEASE_BIN_DIR):
	mkdir -p $(RELEASE_BIN_DIR)

$(DEBUG_OBJ_DIR)/tests:
	mkdir -p $(DEBUG_OBJ_DIR)/tests

$(RELEASE_OBJ_DIR)/tests:
	mkdir -p $(RELEASE_OBJ_DIR)/tests
