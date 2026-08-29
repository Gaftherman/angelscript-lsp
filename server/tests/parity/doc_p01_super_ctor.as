// SCOPE-08, the half of it that is real. `super(args)` calls the base constructor and compiles;
// `super.F()` and `super::F()` do not - the compiler answers "No matching symbol 'super'".
class Base { Base(int x) {} }
class Derived : Base { Derived() { super(1); } }
