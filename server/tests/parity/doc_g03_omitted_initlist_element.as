// PARSER-05. An omitted element takes the type's default. `initializer_list` in the grammar has no
// empty alternative between commas, so the hole makes the declaration an ERROR node.
void main() { array<int> x = { 0, 1, , 4, 5 }; int n = x.length(); print("" + n); }
