// A pattern group with no `repeat` in it wants exactly as many values as it has items.
// `dictionary`'s pattern is {repeat {string, ?}}, so the inner group is a fixed pair:
//
//   ERROR (8, 21): Not enough values to match pattern
//   ERROR (8, 20): Previous error occurred while attempting to compile initialization list for type 'dictionary'
void main()
{
    dictionary d = {{'a'}};
}
