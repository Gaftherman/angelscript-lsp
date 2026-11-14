// @expect: accept
interface IA { void a(); }
interface IB : IA { void b(); }
class C : IB {
    void a() {}
    void b() {}
}
