////////////////////////////////////////////////////////////////////////////
// 3d-test.cc -- small test of SIMLIB/C++ 3D extension
//
// (c) PerPet 1997-2023

#include "simlib.h"
#include "simlib3D.h"
#include <vector>
#include <memory>

typedef Value3D Position, Speed, Force;

const double gravity_constant = 6.67e-11;

// masses:
const double   m0 = 1000;           // satellite [kg]
const double   m1 = 5.983e24;       // Earth
const double   m2 = 7.374e22;       // Moon
// positions:
const Position p0(36.0e6, 0, 0);    // satellite
const Position p2(384.405e6, 0, 0); // Moon
// initial speeds:
const Speed    v0(0, 4200, 1600);   // satellite
const Speed    v2(0, 1022, 0);      // Moon

Constant3D Zero(0,0,0);  // used only for initialization

////////////////////////////////////////////////////////////////////////////
struct MassPoint {
  double m;
  Expression3D inforce;  // gravity force
  Integrator3D v;        // speed
  Integrator3D p;        // position
  MassPoint(const double mass, Position p0, Speed v0=Speed(0,0,0)) :
    m(mass),
    inforce(Zero),
    v(inforce/m, v0),
    p(v,p0) {}
   void SetInput(Input3D i) { inforce.SetInput(i); }
};


////////////////////////////////////////////////////////////////////////////
struct MyWorld {
  std::vector<MassPoint*> m;
  MyWorld();                    // default constructor
  Position Mcenter() {          // center of gravity of the system
    Position p = Value3D(0,0,0);
    double sum = 0;
    for(auto &b: m) {
      p = p + b->m * b->p.Value();
      sum += b->m;
    }
    return p/sum;
  }
};

MyWorld *w; // will be created later

////////////////////////////////////////////////////////////////////////////
// the accelerating force computation block
//
class GravityForce : public aContiBlock3D {
  MassPoint *point;
  MyWorld *world;           // contains all mass points
 public:
  GravityForce(MassPoint *p, MyWorld *w) : point(p), world(w) {}
  Force Value() {
    Force f(0,0,0);
    // compute and sum all gravity forces
    for(auto &m: world->m) {
      if (m == point) continue;     // except point
      Value3D distance = m->p.Value() - point->p.Value();
      double d = abs(distance);
      f = f + (distance * gravity_constant * point->m * m->m / (d*d*d)) ;
    }
    return f;
  }
};

typedef MassPoint Planet, Satelite;

////////////////////////////////////////////////////////////////////////////
// digital world creation (warning: there are memory leaks)
//
MyWorld::MyWorld() {
  m.push_back(new Planet(m1,Position(0,0,0),Speed(0,0,0)));
  m.push_back(new Planet(m2,p2,v2));
  m.push_back(new Satelite(m0,p0,v0));
  for(auto &p: m)         // add gravity forces
    p->SetInput(new GravityForce(p,this));
}

////////////////////////////////////////////////////////////////////////////
// sampling of planet positions
//
void Sample();
Sampler s(Sample,3600);         // sample each hour
void Sample() {
  Position t  = w->Mcenter();   // center of gravity is the origin
  Position p  = (w->m[2])->p.Value() - t;
  Position p1 = (w->m[1])->p.Value() - t;
  Print("%g ", Time);
  Print("%g %g %g ", p1.x()/1e6, p1.y()/1e6, p1.z()/1e6);
  Print("%g %g %g ", p.x()/1e6,  p.y()/1e6,  p.z()/1e6);
  Print("\n");
}

////////////////////////////////////////////////////////////////////////////
// experiment control
//
int main()
{
  w = new MyWorld;
  Print("# Simple model of 3 body problem (Earth, Moon, satellite) \n");
  Init(0, 100000);                // time span
  SetMethod("abm4");
  SetStep(1,50);                  // integration step size limits
  SetAccuracy(1e-5);              // only "demo" precision
  Run();                          // simulation
}

////////////////////////////////////////////////////////////////////////////
