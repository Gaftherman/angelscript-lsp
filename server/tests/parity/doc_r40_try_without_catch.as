// `catch` is not optional, and there is no `finally`.
//
//     angelscript_oracle doc_r40_try_without_catch.as
//         ERROR (16, 1): Expected 'catch'
//
// One of five refused shapes, all measured: a bare `try`, a `finally` instead of a `catch`, a
// `catch (e)` with an exception parameter, two `catch` blocks, and either body written without
// braces. The grammar refuses the same five, which is why none of them needed a rule here.
void test()
{
    try
    {
        int value = 1;
        value = 2;
    }
}
