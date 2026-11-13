# Plain-make build, so the project builds with nothing but a C++20 compiler.
# CMakeLists.txt is the equivalent CMake build.

CXX      ?= c++
STD      := -std=c++20
WARN     := -Wall -Wextra
INCLUDE  := -Isrc -Ibench
OPT      := -O3 -march=native -fno-omit-frame-pointer
BUILD    := build

LIB_SRC  := src/fastOrderBook.cpp
# Most of the code is header-only templates, so every target depends on every
# header; otherwise editing orderBook.h would not trigger a rebuild.
HEADERS  := $(wildcard src/*.h bench/*.h)
SOURCES   = $(filter %.cpp,$^)

.PHONY: all asan clean run-test run-bench
all: $(BUILD)/orderbook_bench $(BUILD)/orderbook_test $(BUILD)/orderbook_demo

$(BUILD):
	@mkdir -p $(BUILD)

# NDEBUG to match CMake's Release build: the asserts sit on the optimized
# engine's hot path only, so leaving them in would skew the comparison against
# it. The test builds keep their asserts.
$(BUILD)/orderbook_bench: bench/benchmark.cpp $(LIB_SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(STD) $(OPT) -DNDEBUG $(WARN) $(INCLUDE) $(SOURCES) -o $@

$(BUILD)/orderbook_test: tests/conformance.cpp $(LIB_SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(STD) -O1 -g $(WARN) $(INCLUDE) $(SOURCES) -o $@

$(BUILD)/orderbook_demo: src/main.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(STD) $(OPT) $(WARN) $(INCLUDE) $(SOURCES) -o $@

# Sanitized test build: the arena and 32-bit handles are exactly where a
# use-after-free would hide, so this is the one that matters.
$(BUILD)/orderbook_test_asan: tests/conformance.cpp $(LIB_SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(STD) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  $(WARN) $(INCLUDE) $(SOURCES) -o $@

# alloc_dealloc_mismatch is OFF by default in Apple's ASan runtime, so a
# new[]/operator-delete mismatch in the pool allocator went unseen on macOS
# while GCC flagged it on Linux. Force it on rather than trust the default:
# appended after any ASAN_OPTIONS already in the environment, since later keys
# win. detect_leaks=0 because LeakSanitizer is unsupported on Apple Silicon and
# aborts the run there.
FORCED_ASAN := alloc_dealloc_mismatch=1:detect_leaks=0

asan: $(BUILD)/orderbook_test_asan
	ASAN_OPTIONS="$$ASAN_OPTIONS:$(FORCED_ASAN)" ./$(BUILD)/orderbook_test_asan 200000

run-test: $(BUILD)/orderbook_test
	./$(BUILD)/orderbook_test 200000

# Bare defaults: the same command CI runs to produce the README numbers.
run-bench: $(BUILD)/orderbook_bench
	./$(BUILD)/orderbook_bench

clean:
	rm -rf $(BUILD)
