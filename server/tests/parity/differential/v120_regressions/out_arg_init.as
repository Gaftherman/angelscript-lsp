// @expect: accept
// @predefined: sdk-addons
void Test(dictionary@ d) {
  float x;
  bool ok = d.get("k", x);
  if (x > 0 && ok) {}
}
