// A lambda takes its parameter types from the funcdef it is assigned to, so it may leave them
// out - and whatever it does write has to match exactly, because the compiler compares the
// written signature rather than converting it. Every line here is ACCEPTED. Measured with:
//
//     angelscript_oracle doc_p27_lambda_funcdef.as
//
// The accepting cases are the point of this file. A rule that compared type names as strings
// would report the last three, and all three are legal:
//
//   * a typedef aliases a primitive, so `real` and `float` are one type spelled two ways;
//   * `array<int>@` and `int[]@` are one type spelled two ways;
//   * a namespaced type may be written bare inside its namespace and qualified outside it.
//
// The rejecting half is doc_r26. See the lambda section of TypeConversionChecker.cpp.

typedef float real;

class Foo {}

namespace N
{
    class Inner {}
    funcdef void InnerCB(Inner@);

    void UsedBare()
    {
        // Bare inside the namespace, and the funcdef writes it bare too.
        InnerCB@ cb = function(Inner@ i) { };
    }
}

funcdef void TakesInt(int);
funcdef void TakesTwo(int, int);
funcdef void TakesConstStringRef(const string &in);
funcdef void TakesOutInt(int &out);
funcdef void TakesFooHandle(Foo@);
funcdef void TakesReal(real);
funcdef void TakesIntArray(array<int>@);
funcdef void HasDefault(int a = 1);
funcdef int ReturnsInt();

void Written()
{
    // Written out and identical.
    TakesInt@ a = function(int v) { };
    TakesConstStringRef@ b = function(const string &in s) { };
    TakesOutInt@ c = function(int &out v) { v = 1; };
    TakesFooHandle@ d = function(Foo@ f) { };

    // A written type needs no name.
    TakesInt@ e = function(int) { };

    // int32 is the explicit spelling of int.
    TakesInt@ f = function(int32 v) { };
}

void Omitted()
{
    // The type comes from the funcdef, which is what lambdas are for.
    TakesInt@ a = function(v) { };
    TakesConstStringRef@ b = function(s) { };
    TakesFooHandle@ c = function(f) { };

    // Partly written is fine too.
    TakesTwo@ d = function(int first, second) { };

    // A default argument belongs to calls; the handle's shape still needs its one parameter.
    HasDefault@ e = function(int v) { };
}

void Spellings()
{
    // A typedef, in both directions.
    TakesReal@ a = function(float v) { };
    TakesReal@ b = function(real v) { };

    // The two array spellings, in both directions.
    TakesIntArray@ c = function(int[]@ v) { };
    TakesIntArray@ d = function(array<int>@ v) { };

    // A namespaced type, qualified from outside.
    N::InnerCB@ e = function(N::Inner@ v) { };
}

void OtherShapes()
{
    // A handle assignment rather than an initializer.
    TakesInt@ a;
    @a = function(int v) { };

    // A funcdef used as a conversion - the shape real code writes.
    Register(TakesInt(function(int v) { }));
    Register(@TakesInt(function(v) { }));

    // The return type is the funcdef's, so the body may return one.
    ReturnsInt@ b = function() { return 1; };
}

void AsArgument()
{
    // The shape real code writes: the lambda lands on a funcdef parameter. Accepted whether the
    // types are written out or left to the funcdef.
    Register(function(int v) { });
    Register(function(v) { });
    RegisterAt(1, function(int v) { });
}

int ReturnsFromEveryPath()
{
    // A non-void funcdef requires every path of the lambda to return, and this one does.
    ReturnsInt@ a = function() { if (true) return 1; return 2; };

    // A void funcdef requires nothing, and a bare return satisfies it.
    TakesInt@ b = function(v) { return; };

    // int widens to float on the way out, the same as anywhere else.
    ReturnsFloat@ c = function() { return 1; };
    return 0;
}

funcdef float ReturnsFloat();

void Register(TakesInt@ cb) {}
void RegisterAt(int at, TakesInt@ cb) {}
