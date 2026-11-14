// @expect: accept
funcdef int Fn(int);
void main() { Fn@ f = function(x) { return x + 1; }; int y = f(2); }
