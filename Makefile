# Plain-make build, so the project builds without installing anything.
# CMakeLists.txt is the equivalent CMake build (needs `brew install cmake`).

CXX      ?= c++
STD      := -std=c++20
WARN     := -Wall -Wextra
INCLUDE  := -Isrc -Ibench
OPT      := -O3 -march=native -fno-omit-frame-pointer
BUILD    := build

LIB_SRC  := src/fastOrderBook.cpp

.PHONY: all bench test demo asan clean run-test run-bench
all: $(BUILD)/orderbook_bench $(BUILD)/orderbook_test $(BUILD)/orderbook_demo

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/orderbook_bench: bench/benchmark.cpp $(LIB_SRC) | $(BUILD)
	$(CXX) $(STD) $(OPT) $(WARN) $(INCLUDE) -DORDERBOOK_HAS_FAST $^ -o $@

$(BUILD)/orderbook_test: tests/conformance.cpp $(LIB_SRC) | $(BUILD)
	$(CXX) $(STD) -O1 -g $(WARN) $(INCLUDE) $^ -o $@

$(BUILD)/orderbook_demo: src/main.cpp | $(BUILD)
	$(CXX) $(STD) $(OPT) $(WARN) $(INCLUDE) $^ -o $@

# Sanitized test build: the arena and 32-bit handles are exactly where a
# use-after-free would hide, so this is the one that matters.
$(BUILD)/orderbook_test_asan: tests/conformance.cpp $(LIB_SRC) | $(BUILD)
	$(CXX) $(STD) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	  $(WARN) $(INCLUDE) $^ -o $@

asan: $(BUILD)/orderbook_test_asan
	./$(BUILD)/orderbook_test_asan 200000


run-test: $(BUILD)/orderbook_test
	./$(BUILD)/orderbook_test 200000

run-bench: $(BUILD)/orderbook_bench
	./$(BUILD)/orderbook_bench --events=3000000 --reps=5

clean:
	rm -rf $(BUILD)
