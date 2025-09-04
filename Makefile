PACKAGE_NAME = eeft_sched
CC = clang
CFLAGS = -Wall -Wextra -std=c11 -Iinclude -pthread
SRC_DIR = src
TARGET_DIR = target
DEBUG_DIR = $(TARGET_DIR)/debug
RELEASE_DIR = $(TARGET_DIR)/release

SRC_FILES = $(wildcard $(SRC_DIR)/*.c)
DEBUG_OBJ_FILES = $(patsubst $(SRC_DIR)/%.c,$(DEBUG_DIR)/%.o,$(SRC_FILES))
RELEASE_OBJ_FILES = $(patsubst $(SRC_DIR)/%.c,$(RELEASE_DIR)/%.o,$(SRC_FILES))

all: release debug

debug: $(DEBUG_DIR)/$(PACKAGE_NAME)

release: $(RELEASE_DIR)/$(PACKAGE_NAME)

run: $(DEBUG_DIR)/$(PACKAGE_NAME)
	$(DEBUG_DIR)/$(PACKAGE_NAME) $(ARGS)

run-release: $(RELEASE_DIR)/$(PACKAGE_NAME)
	$(RELEASE_DIR)/$(PACKAGE_NAME) $(ARGS)

clean:
	rm -rf $(DEBUG_DIR) $(RELEASE_DIR)

$(DEBUG_DIR)/$(PACKAGE_NAME): $(DEBUG_OBJ_FILES)
	$(CC) $(CFLAGS) -g -fsanitize=address -fsanitize=undefined $^ -o $@

$(RELEASE_DIR)/$(PACKAGE_NAME): $(RELEASE_OBJ_FILES)
	$(CC) $(CFLAGS) -O3 $^ -o $@

$(DEBUG_DIR)/%.o: $(SRC_DIR)/%.c | $(DEBUG_DIR)
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined,leak -c $< -o $@

$(RELEASE_DIR)/%.o: $(SRC_DIR)/%.c | $(RELEASE_DIR)
	$(CC) $(CFLAGS) -O3 -c $< -o $@

$(RELEASE_DIR):
	mkdir -p $(RELEASE_DIR)

$(DEBUG_DIR):
	mkdir -p $(DEBUG_DIR)
