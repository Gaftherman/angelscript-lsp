// @expect: reject
funcdef int Fn(int);
void main() { Fn@ f = function(x, y) { return x + y; }; }
