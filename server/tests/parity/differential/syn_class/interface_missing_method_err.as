// @expect: reject
interface IFoo {
    void a();
    void b();
}
class Foo : IFoo {
    void a() {}
}
