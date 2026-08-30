// The rejecting half of doc_p22. A handle on a real primitive is an error, and stays one:
//
//     ERROR: Object handle is not supported for this type
//
// This is the guard that keeps the `auto@` fix from being a blanket exemption - the rule still has
// to fire here.
void main()
{
    int@ handleOnInt;
}
