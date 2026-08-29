// PARSER-10. `T[]` is array<T>, so it carries array<T>'s methods.
void P(int[] list) { int n = list.length(); list.insertLast(10); print("" + n); }
