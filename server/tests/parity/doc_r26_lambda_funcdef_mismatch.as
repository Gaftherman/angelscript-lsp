// The rejecting half of doc_p27. Every lambda below contradicts the funcdef it is assigned to,
// and the compiler answers each one. Measured with:
//
//     angelscript_oracle doc_r26_lambda_funcdef_mismatch.as
//
// Two error shapes, depending on how the lambda reaches the funcdef:
//
//     CB@ cb = function() { };            "Can't implicitly convert from '<auto> lambda()' to 'CB@&'"
//     Register(CB(function() { }));       "No matching signatures to 'CB(<auto> lambda())'"
//
// ARITY is a hard equality even when every parameter is untyped, and a funcdef's default
// argument does not relax it - `funcdef void HasDefault(int a = 1)` still rejects `function()`.
//
// A WRITTEN type does not widen and does not convert: `int` rejects `uint`. The decorations are
// part of the signature too, so `const string &in` rejects both `string` and `string &in`.

class Foo {}

funcdef void TakesInt(int);
funcdef void TakesNothing();
funcdef void TakesConstStringRef(const string &in);
funcdef void TakesOutInt(int &out);
funcdef void TakesFooHandle(Foo@);
funcdef void TakesFooValue(Foo);
funcdef void TakesIntArray(array<int>@);
funcdef void HasDefault(int a = 1);

void WrongArity()
{
    TakesInt@ a = function() { };
    TakesInt@ b = function(x, y) { };
    TakesNothing@ c = function(int v) { };
    HasDefault@ d = function() { };
}

void WrongWrittenType()
{
    TakesInt@ a = function(uint v) { };
    TakesInt@ b = function(string v) { };
}

void WrongDecoration()
{
    TakesConstStringRef@ a = function(string s) { };
    TakesOutInt@ b = function(int v) { };
    TakesFooHandle@ c = function(Foo f) { };
    TakesFooValue@ d = function(Foo@ f) { };
    TakesIntArray@ e = function(int[] v) { };
}

void ThroughAConversion()
{
    Register(TakesInt(function() { }));
}

void AsAnArgument()
{
    // The lambda lands on a funcdef parameter and contradicts it. The compiler answers a call
    // rather than a conversion here: "No matching signatures to 'Register(<auto> lambda())'".
    Register(function() { });
    RegisterAt(1, function(uint v) { });
}

void ReturnType()
{
    // A lambda writes no return type; the funcdef supplies one, and the body has to satisfy it.
    ReturnsInt@ a = function() { };                 // Not all paths return a value
    ReturnsInt@ b = function() { return 'x'; };     // No conversion from 'const string' to 'int'
    TakesNothing@ c = function() { return 1; };     // Can't return value when return type is 'void'
}

funcdef int ReturnsInt();

void Register(TakesInt@ cb) {}
void RegisterAt(int at, TakesInt@ cb) {}
