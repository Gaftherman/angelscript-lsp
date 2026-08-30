// A list on the right of a plain assignment is legal and is compiled against the assignee's type:
//
//   ERROR (10, 10): Can't implicitly convert from 'const string' to 'int&'.
//
// `a += {1};` is a different verdict - "Illegal operation on 'int[]&'" - and is about the operator
// rather than the list, so only `=` is judged here.
void main()
{
    array<int> a;
    a = {"x"};
}
