# IMS project models — build against local SIMLIB tree (../src)
#
# Library: ../src/libsimlib.a is built from SIMLIB sources.
# To rebuild the library from scratch:
#   cd ../src && make -f Makefile.Darwin        # produces simlib.a
# (Apple's /usr/bin/make is GNU Make 3.81 and handles SIMLIB's Makefiles;
#  the top-level wrapper script invokes "gmake", which does not exist here.)
#
CXX      = clang++
CXXFLAGS = -std=c++11 -O2 -Wall
LIBDIR   = ../src
INCLUDE  = -I$(LIBDIR)
LIBS     = -L$(LIBDIR) -lsimlib

MODELS = ore_concentration

all: $(MODELS)

%: %.cc $(LIBDIR)/libsimlib.a
	$(CXX) $(CXXFLAGS) $(INCLUDE) -o $@ $< $(LIBS)

run: $(MODELS)
	@for m in $(MODELS); do echo "== $$m =="; ./$$m; done

plot:
	gnuplot ore_concentration.plt    # if gnuplot is installed

clean:
	rm -f $(MODELS) *.dat *.png
