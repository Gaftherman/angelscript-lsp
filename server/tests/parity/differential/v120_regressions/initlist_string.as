// @expect: accept
// @predefined: sdk-addons
void Print(const string &in fmt, array<string>@ args = null) {}
void Test() {
  float f = 1.0f; int i = 2; uint u = 3;
  Print("a {} {} {}", { f, i, u });
}
