// An anonymous function is an expression, and an expression alone is not a statement.
//
// angelscript_oracle:
//   ERROR (6, 9): Invalid expression: stand-alone anonymous function
//
// The legal spellings are pinned next door in doc_p126: passed to a funcdef parameter, or assigned
// to a funcdef handle. This one is the shape a user reaches for when they think of it as a lambda
// they can just drop into a loop body.

void StandaloneAnonFnRejected()
{
    while (true)
    {
        function(bool param)
        {
        };
    }
}
