////////////////////////////////////////////////////////////////////////////
// Model semaphore-test.cc
//
// Example of queuing system using semaphore (the same as examples/model2.cc)

#include "simlib.h"

Semaphore Box("Server");
Histogram Table("Table of time spent in system",0,25,20);

class Customer : public Process { // Customer description
  double ArrivalTime;             // Attribute for each customer
  void Behavior() {               // - Customer behavior description
    ArrivalTime = Time;           // Mark arrival time
    Box.P();                      // Try to enter Box or queue
    Wait(10);                     // In service
    Box.V();                      // Leave Box
    Table(Time-ArrivalTime);      // Record time spent in system
  }
};

class Generator : public Event {  // Generator of customers
  void Behavior() {               // - Generator behavior description
    (new Customer)->Activate();   // Create new customer, activate at Time
    Activate(Time+Exponential(1e3/150)); // Arrival time interval
  }
};

int main() {                 // Experiment description
  Print("Semaphore test\n");
  Init(0,1000);                 // Init simulator for time 0..1000
  (new Generator)->Activate();  // Create generator, activate at 0
  Run();                        // - Simulation run
  Box.Output();                 // Print results
  Table.Output();
}

