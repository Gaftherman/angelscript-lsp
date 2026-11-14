// @expect: accept
interface IFoo {
    int getVal() const;
}
class Foo : IFoo {
    int getVal() const { return 42; }
}
