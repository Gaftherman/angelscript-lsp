// A loop is not a return, whatever its condition says.
//
//     angelscript_oracle doc_r11_loop_is_not_a_return.as
//         ERROR (9, 1): Not all paths return a value
//
// `while (true)` really has no normal exit, and the compiler does not care: its analysis is
// structural and a loop body may run zero times as far as it is concerned. This analyzer reasoned
// the other way and asserted it in a test, until the test was measured. `for (;;)` and
// `do { ... } while (true)` are rejected the same way.
int Spin()
{
    while (true)
    {
        return 1;
    }
}
