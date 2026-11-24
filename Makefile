# IMS-T8 simulation — build main.cpp only

CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall

all: main

main: main.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

run: main
	./main

clean:
	rm -f main

.PHONY: all run clean
