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

void Register(TakesInt@ cb) {}
