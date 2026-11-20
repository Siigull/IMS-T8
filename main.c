// The language should have for loops, ifs, math functions(like normal dist.),
// state machines inside processors.
// 
// There will be "objects". One mass which is comprised of 
// more materials until they are separated.
// 
// It should contain object processors. Maybe define at the start of 
// the program. (It might need to be hardcoded, we'll see).
// 
// Functions for normal distribution etc.
// 
// Special processors for generation at the start of things, input.
//
// ? How to do visualization? We can prompt something ig.
//
// The language will be compiled into a discrete time simulation.
// - time step and a loop.
// 
// Processors don't send output just "forward", but they could need to send
// output to previous, in the chain, processors.
// 
// Bidirectional failure for processors. If something upwards in chain, the
// previous can't work because they can't output.
//
// Some failures are expected behavior, what happens at failure will be defined also.
// If a mechanic needs to come, mechanic is an input too. (Time of day too?). Emergency shifts?
//
// There will be longer term storages. How to map them as processors.
// ? How to map them. The transfer will take time, and has limited capacity.
//
// ? Have the simulation find out sizes for some parts based on input
//
// ! Processors need maintenance.
//
// Define stats for processors. Don't know, if they should be hard set for all,
// or if we'll need to define them in language.
//

// root -> (statement + ";")*
// statement -> object_decl, object_init, link
// object_decl -> name + "(" "in:" + "[" + params + "]" + "out:" + "[" + params + "]" + ")" + "{" + object_body + "}"
// params -> (param + ",")*
// name -> id
// param -> id
// object_body -> (prog_statement + ";")*
// object_init -> id + "=" + name
// link -> id "->" id + "{" + ("[" + id + "," + id + "]")* "}"

// statement -> for, if
// for -> "for" + id + "in" + "[" + (id + ",")* + "]" + "{" + for_body + "}"

enum {
    AST_OBJECT,
    AST_LOOP,
    AST_IF,
}

typedef struct AST {
    union as {

    }
}

int main() {

}