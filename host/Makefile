# uwbnetd: TUN <-> uwbnet board bridge. Shares the COBS code with the firmware.
#   make -C host

CXX ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -I../firmware/src

uwbnetd: uwbnetd.cpp ../firmware/src/cobs.cpp ../firmware/src/cobs.h
	$(CXX) $(CXXFLAGS) -o $@ uwbnetd.cpp ../firmware/src/cobs.cpp

.PHONY: clean
clean:
	rm -f uwbnetd
