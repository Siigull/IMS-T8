////////////////////////////////////////////////////////////////////////////
// random-test.cc - simple test of Random() generator in SIMLIB/C++
//

#include "simlib.h"

int main() {
  Print("Test of internal random generator of SIMLIB/C++\n");
  SetOutput("random-test.out");
  for(int i=0; i<100; i++)
      Print("%g\n",Random());
}

