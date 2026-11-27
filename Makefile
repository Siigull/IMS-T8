# IMS-T8 simulation — sim + trace viewer

CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall

RAYLIB_DIR = third_party/raylib
RAYLIB_INC = $(RAYLIB_DIR)/include
RAYLIB_LIB = $(RAYLIB_DIR)/lib/libraylib.a
RAYLIB_LDFLAGS = -lGL -lm -lpthread -ldl -lrt -lX11

all: main viewer

main: main.cpp sim_trace.h trace_format.h
	$(CXX) $(CXXFLAGS) -o $@ main.cpp -lz

viewer: viewer.cpp trace_format.h $(RAYLIB_LIB)
	$(CXX) $(CXXFLAGS) -o $@ viewer.cpp -I$(RAYLIB_INC) $(RAYLIB_LIB) $(RAYLIB_LDFLAGS) -lz

$(RAYLIB_LIB):
	@echo "raylib missing — see third_party/raylib/README.md"

run: main
	./main

clean:
	rm -f main viewer

.PHONY: all run clean
