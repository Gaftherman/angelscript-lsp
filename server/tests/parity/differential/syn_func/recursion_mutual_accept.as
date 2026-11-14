// @expect: accept
bool isEven(int n) { if (n == 0) return true; return isOdd(n - 1); }
bool isOdd(int n) { if (n == 0) return false; return isEven(n - 1); }
void main() { bool b = isEven(4); }
