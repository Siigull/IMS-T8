
// test of process reactivation
// Passivate, Activate, Terminate

#include "simlib.h"

// test process
class TestP: public Process {
  public:
    void Behavior() {
        Print("time=%g p1\n", Time);
        Passivate();
        Print("time=%g p2\n", Time);
        Passivate();
        Print("time=%g p3\n", Time);
        Passivate();
        Print("time=%g p4\n", Time);
        Terminate();
        Print("time=%g p5\n", Time);
    }
};

// event to (re)schedule process
class TestE:public Event {
    Process * ptr;
  public:
    TestE(Process*p): ptr(p) {}
    void Behavior() {
        ptr->Activate(Time+0.1);
    }
};


int main()
{
//    DebugON();
    Print("SIMLIB/C++ test of re-scheduling\n");
    Init(0, 5);
    TestP *p = new TestP;
    p->Activate();
    (new TestE(p))->Activate(1);
    (new TestE(p))->Activate(1);
    (new TestE(p))->Activate(2);
    (new TestE(p))->Activate(3);
    // (new TestE(p))->Activate(4); // p deleted after Terminate
    Run();
}
