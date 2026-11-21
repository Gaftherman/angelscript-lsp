// @expect: accept
class Base { void Reload(int a, float b) {} }
class Weapon : Base {
  void Tick(int clip) {
    (clip == 0) ? Reload(1, 2.0f) : Reload(2, 3.0f);
  }
}
