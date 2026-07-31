// Test all grammar node types we need to audit
void global_func(int a, float b) {}
int global_var;
int@ handle_var;
int[]@ array_handle;

class MyClass : BaseClass {
    int field;
    void method() {}
}

final class FinalClass {}
abstract class AbstractClass {}
mixin class MyMixin {}

interface IMyInterface {
    void method();
}

enum Color { Red = 1, Green = 2 }

typedef int MyInt;
funcdef void Callback(int x);

namespace MyNS {
    void ns_func() {}
    int ns_var;
}
