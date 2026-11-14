// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(2, 20, 'Unable to resolve auto type')]
// @expect: reject
void main() { auto f = function(int x) { return x; }; }
