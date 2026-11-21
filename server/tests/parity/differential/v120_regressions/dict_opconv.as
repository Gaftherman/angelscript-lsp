// @expect: accept
// @predefined: sdk-addons
class Holder { int v; }
void Test(dictionary@ d) {
  int a = int(d["k"]);
  string s = string(d["k"]);
}
