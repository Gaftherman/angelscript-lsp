// ===== CASE 1: Variable types =====
// Primitive
int x;
float y;
bool flag;

// Primitive handle (invalid in AngelScript)
int@ bad;

// Class handle
Player@ p;

// Array of primitive
array<int> arr1;
int[] arr2;

// Array handle (valid - handle to an array)
array<int>@ arr3;
int[]@ arr4;

// Array of handles
array<Player@> arr5;

// Nested arrays
array<array<int>> arr6;
array<array<int>@> arr7;

// Primitive handle inside array (invalid)
array<int@> arr8bad;
int@[]@ arr9bad;

// ===== CASE 2: Functions =====
void Func1() {}
int Func2(int a, float b) {}
Player@ Func3() {}
array<int>@ Func4() {}
SomeFuncDef@ Func5() {}
SomeFuncDef Func6() {}

// Parameters
void Func7(int a, float& b, Player@ c, out int d, const int e, inout float f) {}
void Func8(array<int>@ arr, int[]@ arr2) {}
void Func9(int a = 5, float b = 3.14f) {}

// ===== CASE 3: Classes =====
class MyClass {}
class Child : Parent {}
class MultiInherit : IFoo, IBar {}
class Final final {}
class Abstract abstract {}
shared class Shared {}
class Template<T> {}

// ===== CASE 4: Typedefs =====
typedef int MyInt;
typedef float MyFloat;

// ===== CASE 5: Funcdefs =====
funcdef void Callback();
funcdef int Predicate(int a, float b);

// ===== CASE 6: Namespace =====
namespace MyNS {}
namespace Outer::Inner {}

// ===== CASE 7: Enum =====
enum Color { Red, Green, Blue }
