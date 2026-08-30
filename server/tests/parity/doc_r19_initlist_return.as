// `return {1, 2};` compiles: the list is built against the declared return type. With a bad element
// the compiler answers
//
//   ERROR (9, 16): Can't implicitly convert from 'const string' to 'int&'.
//
// Nothing here was visited before, so the whole return position went unjudged.
array<int> Make()
{
    return {1, "x"};
}
