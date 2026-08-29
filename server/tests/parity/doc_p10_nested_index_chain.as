// TYPE-08. opIndex's element type has to survive a member chain, including an index used as an
// index.
class R { float mins; }
class M { array<R> re; array<int> ch; }
class E { M mi; }
void T(E@ self) { R@ r = self.mi.re[self.mi.ch[0]]; float m = r.mins; print("" + m); }
