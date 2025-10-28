.PHONY: all debug release profile run run-release run-profile test test-release test-profile clean show-flags

PACKAGE_NAME = eeft_sched

CC = clang

CFLAGS = -Wall -Wextra -std=c11 -Iinclude -pthread -MMD -MP

CFLAGS += -Wvla -Wfloat-equal -Wshadow -Wpointer-arith -Wno-cast-align \
          -Wstrict-prototypes -Wmissing-prototypes -Wreturn-type

STRICT ?= 0
ifeq ($(STRICT),1)
CFLAGS += -Werror -pedantic -Wconversion
endif

V ?= 0
ifeq ($(V), 0)
    Q = @
else
    Q =
endif

SRC_DIR = src
TEST_DIR = tests
TARGET_DIR = target

DEBUG_DIR = $(TARGET_DIR)/debug
RELEASE_DIR = $(TARGET_DIR)/release
PROFILE_DIR = $(TARGET_DIR)/profile

DEBUG_OBJECTS_DIR = $(DEBUG_DIR)/objects
RELEASE_OBJECTS_DIR = $(RELEASE_DIR)/objects
PROFILE_OBJECTS_DIR = $(PROFILE_DIR)/objects

DEBUG_BIN_DIR = $(DEBUG_DIR)/bin
RELEASE_BIN_DIR = $(RELEASE_DIR)/bin
PROFILE_BIN_DIR = $(PROFILE_DIR)/bin

COLOR_RESET   := \033[0m
COLOR_INFO    := \033[1;34m
COLOR_OK      := \033[1;32m
COLOR_WARN    := \033[1;33m
COLOR_FAIL    := \033[1;31m
COLOR_DIM     := \033[2m

define log_info
	@printf "$(COLOR_INFO)%s$(COLOR_RESET)\n" "$1"
endef

define log_ok
	@printf "$(COLOR_OK)%s$(COLOR_RESET)\n" "$1"
endef

define log_warn
	@printf "$(COLOR_WARN)%s$(COLOR_RESET)\n" "$1"
endef

define log_fail
	@printf "$(COLOR_FAIL)%s$(COLOR_RESET)\n" "$1"
endef

define MSG_CC
	@printf "$(COLOR_INFO)[CC]$(COLOR_RESET)      $(COLOR_DIM)%s$(COLOR_RESET) -> $(COLOR_OK)%s$(COLOR_RESET)\n" "$1" "$2"
endef

define MSG_CC_TEST
	@printf "$(COLOR_INFO)[CC-TEST]$(COLOR_RESET)  $(COLOR_DIM)%s$(COLOR_RESET) -> $(COLOR_OK)%s$(COLOR_RESET)\n" "$1" "$2"
endef

define MSG_LD
	@printf "$(COLOR_INFO)[LD]$(COLOR_RESET)       $(COLOR_OK)%s$(COLOR_RESET)\n" "$1"
endef

define MSG_SUB
	@printf "      $(COLOR_DIM)-> %s...$(COLOR_RESET)\n" "$1"
endef


SRC_FILES = $(wildcard $(SRC_DIR)/*.c)
TEST_FILES = $(wildcard $(TEST_DIR)/*.c)


DEBUG_OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(DEBUG_OBJECTS_DIR)/%.o,$(SRC_FILES))
RELEASE_OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(RELEASE_OBJECTS_DIR)/%.o,$(SRC_FILES))
PROFILE_OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(PROFILE_OBJECTS_DIR)/%.o,$(SRC_FILES))

DEBUG_MAIN_OBJ = $(DEBUG_OBJECTS_DIR)/main.o
RELEASE_MAIN_OBJ = $(RELEASE_OBJECTS_DIR)/main.o
PROFILE_MAIN_OBJ = $(PROFILE_OBJECTS_DIR)/main.o

DEBUG_OTHER_OBJS = $(filter-out $(DEBUG_MAIN_OBJ), $(DEBUG_OBJECTS))
RELEASE_OTHER_OBJS = $(filter-out $(RELEASE_MAIN_OBJ), $(RELEASE_OBJECTS))
PROFILE_OTHER_OBJS = $(filter-out $(PROFILE_MAIN_OBJ), $(PROFILE_OBJECTS))

DEBUG_TEST_OBJECTS = $(patsubst $(TEST_DIR)/%.c,$(DEBUG_OBJECTS_DIR)/tests/%.o,$(TEST_FILES))
RELEASE_TEST_OBJECTS = $(patsubst $(TEST_DIR)/%.c,$(RELEASE_OBJECTS_DIR)/tests/%.o,$(TEST_FILES))
PROFILE_TEST_OBJECTS = $(patsubst $(TEST_DIR)/%.c,$(PROFILE_OBJECTS_DIR)/tests/%.o,$(TEST_FILES))


all: release debug profile

debug: $(DEBUG_BIN_DIR)/$(PACKAGE_NAME) $(DEBUG_BIN_DIR)/$(PACKAGE_NAME)-tests

release: $(RELEASE_BIN_DIR)/$(PACKAGE_NAME) $(RELEASE_BIN_DIR)/$(PACKAGE_NAME)-tests

profile: $(PROFILE_BIN_DIR)/$(PACKAGE_NAME) $(PROFILE_BIN_DIR)/$(PACKAGE_NAME)-tests


run: $(DEBUG_BIN_DIR)/$(PACKAGE_NAME)
	$(call log_info, Running debug binary...)
	$Q$(DEBUG_BIN_DIR)/$(PACKAGE_NAME) $(ARGS)

run-release: $(RELEASE_BIN_DIR)/$(PACKAGE_NAME)
	$(call log_info, Running release binary...)
	$Q$(RELEASE_BIN_DIR)/$(PACKAGE_NAME) $(ARGS)

run-profile: $(PROFILE_BIN_DIR)/$(PACKAGE_NAME)
	$(call log_info, Running profile binary...)
	$Q$(PROFILE_BIN_DIR)/$(PACKAGE_NAME) $(ARGS)


test: $(DEBUG_BIN_DIR)/$(PACKAGE_NAME)-tests
	$(call log_info, Running debug tests...)
	$Q$(DEBUG_BIN_DIR)/$(PACKAGE_NAME)-tests $(ARGS)

test-release: $(RELEASE_BIN_DIR)/$(PACKAGE_NAME)-tests
	$(call log_info, Running release tests...)
	$Q$(RELEASE_BIN_DIR)/$(PACKAGE_NAME)-tests $(ARGS)

test-profile: $(PROFILE_BIN_DIR)/$(PACKAGE_NAME)-tests
	$(call log_info, Running profile tests...)
	$Q$(PROFILE_BIN_DIR)/$(PACKAGE_NAME)-tests $(ARGS)


clean:
	$(call log_info, Cleaning $(TARGET_DIR)...)
	$Qrm -rf $(RELEASE_DIR) $(DEBUG_DIR) $(PROFILE_DIR)
	$(call log_ok, Clean complete.)


$(DEBUG_BIN_DIR)/$(PACKAGE_NAME): $(DEBUG_OBJECTS) | $(DEBUG_BIN_DIR)
	$(call MSG_LD, $@)
	$Q$(CC) $(CFLAGS) -g -fsanitize=address,undefined $^ -o $@
	$(call MSG_SUB, dSYM)
	$Qdsymutil $@
	$(call log_ok, Built debug: $@)

$(RELEASE_BIN_DIR)/$(PACKAGE_NAME): $(RELEASE_OBJECTS) | $(RELEASE_BIN_DIR)
	$(call MSG_LD, $@)
	$Q$(CC) $(CFLAGS) -O3 $^ -o $@
	$(call MSG_SUB, strip)
	$Qstrip $@
	$(call log_ok, Built release: $@)

$(PROFILE_BIN_DIR)/$(PACKAGE_NAME): $(PROFILE_OBJECTS) | $(PROFILE_BIN_DIR)
	$(call MSG_LD, $@)
	$Q$(CC) $(CFLAGS) -O3 -g $^ -o $@
	$(call MSG_SUB, dSYM)
	$Qdsymutil $@
	$(call log_ok, Built profile: $@)


$(DEBUG_BIN_DIR)/$(PACKAGE_NAME)-tests: $(DEBUG_OTHER_OBJS) $(DEBUG_TEST_OBJECTS) | $(DEBUG_BIN_DIR)
	$(call MSG_LD, $@)
	$Q$(CC) $(CFLAGS) -g -fsanitize=address,undefined $^ -o $@
	$(call log_ok, Built debug tests: $@)

$(RELEASE_BIN_DIR)/$(PACKAGE_NAME)-tests: $(RELEASE_OTHER_OBJS) $(RELEASE_TEST_OBJECTS) | $(RELEASE_BIN_DIR)
	$(call MSG_LD, $@)
	$Q$(CC) $(CFLAGS) -O3 $^ -o $@
	$(call MSG_SUB, strip)
	$Qstrip $@
	$(call log_ok, Built release tests: $@)

$(PROFILE_BIN_DIR)/$(PACKAGE_NAME)-tests: $(PROFILE_OTHER_OBJS) $(PROFILE_TEST_OBJECTS) | $(PROFILE_BIN_DIR)
	$(call MSG_LD, $@)
	$Q$(CC) $(CFLAGS) -O3 -g $^ -o $@
	$(call MSG_SUB, dSYM)
	$Qdsymutil $@
	$(call log_ok, Built profile tests: $@)


$(DEBUG_OBJECTS_DIR)/%.o: $(SRC_DIR)/%.c | $(DEBUG_OBJECTS_DIR)
	$(call MSG_CC, $<, $@)
	$Q$(CC) $(CFLAGS) -g -fsanitize=address,undefined -c $< -o $@

$(RELEASE_OBJECTS_DIR)/%.o: $(SRC_DIR)/%.c | $(RELEASE_OBJECTS_DIR)
	$(call MSG_CC, $<, $@)
	$Q$(CC) $(CFLAGS) -O3 -c $< -o $@

$(PROFILE_OBJECTS_DIR)/%.o: $(SRC_DIR)/%.c | $(PROFILE_OBJECTS_DIR)
	$(call MSG_CC, $<, $@)
	$Q$(CC) $(CFLAGS) -O3 -g -c $< -o $@


$(DEBUG_OBJECTS_DIR)/tests/%.o: $(TEST_DIR)/%.c | $(DEBUG_OBJECTS_DIR)/tests
	$(call MSG_CC_TEST, $<, $@)
	$Q$(CC) $(CFLAGS) -g -fsanitize=address,undefined -c $< -o $@

$(RELEASE_OBJECTS_DIR)/tests/%.o: $(TEST_DIR)/%.c | $(RELEASE_OBJECTS_DIR)/tests
	$(call MSG_CC_TEST, $<, $@)
	$Q$(CC) $(CFLAGS) -O3 -c $< -o $@

$(PROFILE_OBJECTS_DIR)/tests/%.o: $(TEST_DIR)/%.c | $(PROFILE_OBJECTS_DIR)/tests
	$(call MSG_CC_TEST, $<, $@)
	$Q$(CC) $(CFLAGS) -O3 -g -c $< -o $@


$(DEBUG_OBJECTS_DIR) $(RELEASE_OBJECTS_DIR) $(PROFILE_OBJECTS_DIR):
	$Q@mkdir -p $@

$(DEBUG_OBJECTS_DIR)/tests $(RELEASE_OBJECTS_DIR)/tests $(PROFILE_OBJECTS_DIR)/tests:
	$Q@mkdir -p $@

$(DEBUG_BIN_DIR) $(RELEASE_BIN_DIR) $(PROFILE_BIN_DIR):
	$Q@mkdir -p $@


-include $(DEBUG_OBJECTS_DIR)/*.d
-include $(RELEASE_OBJECTS_DIR)/*.d
-include $(PROFILE_OBJECTS_DIR)/*.d
-include $(DEBUG_OBJECTS_DIR)/tests/*.d
-include $(RELEASE_OBJECTS_DIR)/tests/*.d
-include $(PROFILE_OBJECTS_DIR)/tests/*.d


show-flags:
	@printf "CC = %s\n" "$(CC)"
	@printf "CFLAGS = %s\n" "$(CFLAGS)"
	@printf "STRICT = %s\n" "$(STRICT)"
	@printf "V = %s\n" "$(V)"
