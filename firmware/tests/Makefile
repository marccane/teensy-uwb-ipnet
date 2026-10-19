# Host-side unit tests for the hardware-independent firmware modules.
#   make -C firmware/tests

CXX ?= g++
CXXFLAGS = -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I../src
SRCS = test_main.cpp ../src/cobs.cpp ../src/frame.cpp ../src/phy.cpp ../src/config.cpp
BUILD = build

.PHONY: test clean
test: $(BUILD)/unit_tests
	./$(BUILD)/unit_tests

$(BUILD)/unit_tests: $(SRCS) $(wildcard ../src/*.h)
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

clean:
	rm -rf $(BUILD)
