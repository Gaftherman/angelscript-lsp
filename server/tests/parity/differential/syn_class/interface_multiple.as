// @expect: accept
interface IA { void a(); }
interface IB { void b(); }
class C : IA, IB {
    void a() {}
    void b() {}
}
