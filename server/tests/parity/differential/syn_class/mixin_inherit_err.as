// @expect: reject
class Base {}
mixin class MyMixin : Base {}
class C : MyMixin {}
