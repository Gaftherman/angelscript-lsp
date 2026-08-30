// `auto` sits in the grammar's `primitive_type` node beside `int` and `float`, and the rule that
// rejects a handle on a primitive read the node rather than the name. So `auto@` - the ordinary
// way to declare a deduced handle - was reported as an error on code that compiles.
//
// The compiler's two answers, which is what separates them:
//
//     int@ x;                 ERROR: Object handle is not supported for this type
//     auto@ g = MakeFoo();    accepted
//
// `auto` is not a type at all, it is a placeholder for whatever the initializer produces, so
// whether a handle is allowed is decided by that type and never by `auto`. Found by the corpus
// audit: two files in the 1,061-script corpus use it, and both were being reported.
// The rejecting half is doc_r24.
class Foo
{
    int value;
}

Foo@ MakeFoo()
{
    return Foo();
}

void main()
{
    auto@ deduced = MakeFoo();
    deduced.value = 1;
}
