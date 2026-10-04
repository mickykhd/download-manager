# Simple POSIX build (Linux/macOS). For Windows or static curl, use CMake.
CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -Iinclude -Isrc/engine
CFLAGS  += $(shell pkg-config --cflags libcurl)
LDFLAGS += $(shell pkg-config --libs libcurl) -lpthread

BUILD := build
ENGINE_SRC := \
	src/engine/download.c \
	src/engine/httpinfo.c \
	src/engine/segment.c \
	src/engine/scheduler.c \
	src/engine/retry.c \
	src/engine/resume.c \
	src/engine/speedmeter.c \
	src/engine/integrity.c \
	src/platform/platform_posix.c

ENGINE_OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(ENGINE_SRC))

.PHONY: all clean test
all: $(BUILD)/cdm

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/cdm: $(ENGINE_OBJ) $(BUILD)/cli/main.o
	$(CC) $(ENGINE_OBJ) $(BUILD)/cli/main.o -o $@ $(LDFLAGS)

$(BUILD)/cli/main.o: src/cli/main.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

test: $(ENGINE_OBJ) $(BUILD)/tests/test_range.o $(BUILD)/ui/manager.o $(BUILD)/tests/test_settings.o
	$(CC) $(ENGINE_OBJ) $(BUILD)/tests/test_range.o -o $(BUILD)/test_range $(LDFLAGS)
	$(BUILD)/test_range
	$(CC) $(ENGINE_OBJ) $(BUILD)/ui/manager.o $(BUILD)/tests/test_settings.o -o $(BUILD)/test_settings $(LDFLAGS)
	$(BUILD)/test_settings

$(BUILD)/tests/test_range.o: tests/test_range.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ui/manager.o: src/ui/manager.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tests/test_settings.o: tests/test_settings.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isrc/ui -c $< -o $@

clean:
	rm -rf $(BUILD)
