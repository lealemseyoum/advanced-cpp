# Plain Makefile (the course skips build-system chapters). GCC 13.3 or newer; Clang 18 + libstdc++ 13 lacks std::expected.
CXX      ?= g++
STD      := -std=c++23
WARN     := -Wall -Wextra
INC      := -Iinclude
BUILD    := build
TESTS    := loop runtime http

.PHONY: all test test-asan test-tsan bench clean
all: $(BUILD)/server

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/server: examples/server.cpp $(wildcard include/aio/*.hpp) | $(BUILD)
	$(CXX) $(STD) -O2 $(WARN) $(INC) $< -o $@ -pthread

# --- tests, under each sanitizer configuration -------------------------------------------------------------------
test-asan: | $(BUILD)
	@for t in $(TESTS); do $(CXX) $(STD) -O2 -g $(WARN) $(INC) -fsanitize=address,undefined tests/test_$$t.cpp -o $(BUILD)/test_$${t}_asan -pthread && echo "== $$t (ASan+UBSan)" && $(BUILD)/test_$${t}_asan || exit 1; done

test-tsan: | $(BUILD)
	@for t in $(TESTS); do $(CXX) $(STD) -O2 -g $(WARN) $(INC) -fsanitize=thread tests/test_$$t.cpp -o $(BUILD)/test_$${t}_tsan -pthread && echo "== $$t (TSan)" && $(BUILD)/test_$${t}_tsan || exit 1; done

test: test-asan test-tsan

bench: | $(BUILD)
	$(CXX) $(STD) -O2 $(INC) bench/bench_parse.cpp -o $(BUILD)/bench_parse -pthread && $(BUILD)/bench_parse
	$(CXX) $(STD) -O2 $(INC) -I.. ../p10/loadgen.cpp -o $(BUILD)/loadgen -pthread
	@echo "start:  $(BUILD)/server 8080 2     then:  $(BUILD)/loadgen 8080 100 2000"

clean:
	rm -rf $(BUILD)
