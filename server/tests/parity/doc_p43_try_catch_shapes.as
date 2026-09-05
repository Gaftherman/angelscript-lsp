// The shapes of try/catch the language accepts, which turned out to be exactly the shapes the
// grammar accepts.
//
//     angelscript_oracle doc_p43_try_catch_shapes.as   accepted
//
// Written because the grammar rule looked suspiciously strict - `seq("try", statement_block,
// "catch", statement_block)`, one catch, no exception parameter, both bodies braced - and a grammar
// stricter than the language costs a SYMBOL rather than producing a diagnostic: the declaration
// collapses into an ERROR node and leaves the index, so nothing in it can be hovered or completed
// and no rule ever runs on it. Measured in eight shapes; the grammar was right, and doc_r40 carries
// one of the five it refuses.
void nested()
{
    try
    {
        try
        {
            int inner = 1;
            inner = 2;
        }
        catch
        {
        }
    }
    catch
    {
    }
}
