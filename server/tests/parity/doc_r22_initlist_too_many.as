// The other direction of the same rule:
//
//   ERROR (7, 21): Too many values to match pattern
//   ERROR (7, 20): Previous error occurred while attempting to compile initialization list for type 'dictionary'
void main()
{
    dictionary d = {{'a', 1, 2}};
}
