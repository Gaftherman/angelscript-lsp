// SCOPE-08 as the document states it, refuted. The compiler answers "No matching symbol 'super'".
// `super` exists only as the base-constructor call; the idiom for a base method is `Base::F()`.
class B { void F() {} }
class D : B { void F() { super.F(); } }
