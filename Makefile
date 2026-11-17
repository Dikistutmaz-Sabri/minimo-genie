# Builds against the installed Hyprland's headers (pacman: hyprland). Rebuild after every Hyprland update.
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++26 -shared -fPIC -Wall --no-gnu-unique $(shell pkg-config --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon) -I/usr/include

all: minimo-genie.so

minimo-genie.so: src/main.cpp src/genie_math.hpp
	$(CXX) $(CXXFLAGS) src/main.cpp -o $@

check:
	$(CXX) -std=c++20 -Isrc test/math_test.cpp -o test/math_test && ./test/math_test

clean:
	rm -f minimo-genie.so test/math_test

.PHONY: all check clean
