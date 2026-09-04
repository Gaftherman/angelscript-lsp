// The accepting side of the return-value rules, and the reason each of them is narrow.
//
//     angelscript_oracle doc_p13_return_conversions.as   accepted
//
// Every one of these is a conversion the compiler performs silently. A return-type rule that could
// not tell them from the rejecting cases beside them would report ordinary code, which is the one
// thing this project does not do.
enum Color { Red, Green }

class Base {}
class Derived : Base {}

float widening() { return 42; }
int64 promoting(int8 v) { return v; }
int fromEnum() { return Red; }
Base@ upcast(Derived@ d) { return d; }
int fromComparisonBranch(int v) { return v > 0 ? 1 : 0; }

int bothBranches(int v)
{
    if (v > 0)
        return 1;
    else
        return 0;
}
