# Zeebo LLE debug harness + device models (C++23) — host build
CXX ?= g++
CXXFLAGS ?= -std=c++23 -O2 -Wall -Wextra
LIBS := -lunicorn -lcapstone

all: zeebo_harness zeebo_devices_test

zeebo_harness: zeebo_harness.cpp zeebo_devices.h
	$(CXX) $(CXXFLAGS) -o $@ zeebo_harness.cpp $(LIBS)

zeebo_devices_test: zeebo_devices_test.cpp zeebo_devices.h
	$(CXX) $(CXXFLAGS) -o $@ zeebo_devices_test.cpp -lunicorn

test: zeebo_devices_test
	./zeebo_devices_test

clean:
	rm -f zeebo_harness zeebo_devices_test

.PHONY: all test clean
zeebo_partition: zeebo_partition.cpp zeebo_devices.h
	$(CXX) $(CXXFLAGS) -o $@ zeebo_partition.cpp -lunicorn
