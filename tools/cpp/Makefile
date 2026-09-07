# Zeebo LLE debug harness (C++23) — host build
CXX ?= g++
CXXFLAGS ?= -std=c++23 -O2 -Wall -Wextra
LIBS := -lunicorn -lcapstone

zeebo_harness: zeebo_harness.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f zeebo_harness

.PHONY: clean