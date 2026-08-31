// `Foo obj(bar);` is a variable declared with a direct initialization, not a function declaration.
// C++ reads the same text the other way - the most vexing parse - and AngelScript does not.
// Every line here is ACCEPTED:
//
//     angelscript_oracle doc_p31_direct_initialization.as
//
// This was confirmed correct and left untested, which is a worse state than untested-and-unknown:
// the grammar settles it with `prec.dynamic(2)` on `func_declaration` against `1` on
// `variable_declaration`, and a precedence written for a reason with no case pinning it is one
// refactor away from being tuned back. The shapes below are the ones that would break first.

class Bar
{
    Bar() {}
}

class Foo
{
    Foo() {}
    Foo(int value) {}
    Foo(int first, int second) {}
    Foo(Bar other) {}
    Foo(const string &in name) {}
}

int g_bar = 7;

void Declarations()
{
    // The shape itself: an argument that is a plain identifier, which is what makes the text
    // ambiguous to a C++ parser - `Foo obj(bar)` could be read as declaring a function `obj`
    // taking one parameter of type `bar`.
    int bar = 1;
    Foo fromLocal(bar);
    Foo fromGlobal(g_bar);

    // Empty parentheses, the case C++ reads as a function declaration outright.
    Foo empty();

    // More than one argument, and a nested construction as an argument.
    Foo two(1, 2);
    Foo nested(Bar());

    // A literal argument, which is unambiguous, kept as the control.
    Foo literal(3);
    Foo named("weapon");
}

class Holder
{
    // The same declaration as a class member, where the enclosing context differs.
    Foo member(1);
}

void Nested()
{
    // And inside a nested block, where the statement context differs again.
    if (true)
    {
        int bar = 2;
        Foo inner(bar);
    }
}
