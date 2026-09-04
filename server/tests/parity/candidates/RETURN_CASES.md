### CASE 1: Float literal returned from int function
EXPECT: reject
WHY: AngelScript disallows implicit narrowing conversions from float to int in return statements.
```angelscript
int test()
{
    return 3.14f;
}
```

### CASE 2: Int literal returned from float function
EXPECT: accept
WHY: AngelScript permits implicit widening conversions from int to float in return statements.
```angelscript
float test()
{
    return 42;
}
```

### CASE 3: Int8 returned from int64 function
EXPECT: accept
WHY: AngelScript permits implicit widening conversion from int8 to int64 in return statements.
```angelscript
int64 test(int8 val)
{
    return val;
}
```

### CASE 4: Class instance returned from primitive int function
EXPECT: reject
WHY: A user-defined class value cannot be implicitly converted to a primitive int return type.
```angelscript
class Item {}

int test()
{
    Item item;
    return item;
}
```

### CASE 5: Null returned from primitive int function
EXPECT: reject
WHY: The null literal is only valid for handle types and cannot be returned from a primitive int function.
```angelscript
int test()
{
    return null;
}
```

### CASE 6: Call to void function used as return value in non-void function
EXPECT: reject
WHY: An expression of type void cannot be used as the return value of a non-void function.
```angelscript
void performWork() {}

int test()
{
    return performWork();
}
```

### CASE 7: Null returned from handle function
EXPECT: accept
WHY: Null is a valid handle literal representing an unassigned object handle.
```angelscript
class Node {}

Node@ test()
{
    return null;
}
```

### CASE 8: Value instance returned from handle function
EXPECT: reject
WHY: A value instance cannot be implicitly converted to an object handle return type without handle syntax.
```angelscript
class Node {}

Node@ test()
{
    Node n;
    return n;
}
```

### CASE 9: Const reference returned from class method
EXPECT: accept
WHY: A class method can return a const reference to a member variable.
```angelscript
class ValueHolder
{
    int m_val;
    const int& getVal() const
    {
        return m_val;
    }
}
```

### CASE 10: Derived class handle returned from base class handle function
EXPECT: accept
WHY: A handle to a derived class implicitly upcasts to a handle to its base class upon return.
```angelscript
class Base {}
class Derived : Base {}

Base@ test(Derived@ d)
{
    return d;
}
```

### CASE 11: Base class handle returned from derived class handle function
EXPECT: reject
WHY: Downcasting from a base class handle to a derived class handle cannot be performed implicitly.
```angelscript
class Base {}
class Derived : Base {}

Derived@ test(Base@ b)
{
    return b;
}
```

### CASE 12: Implementing class handle returned from interface handle function
EXPECT: accept
WHY: A class handle implicitly converts to an interface handle that the class implements.
```angelscript
interface IHandler {}
class Handler : IHandler {}

IHandler@ test(Handler@ h)
{
    return h;
}
```

### CASE 13: Array of floats returned from array of ints function
EXPECT: reject
WHY: Incompatible template specializations like array<float> and array<int> cannot be implicitly converted.
```angelscript
array<int> test(array<float> input)
{
    return input;
}
```

### CASE 14: Initializer list returned from array function
EXPECT: accept
WHY: AngelScript allows returning an initialization list to construct the expected array return type.
```angelscript
array<int> test()
{
    return {1, 2, 3};
}
```

### CASE 15: Value array returned where array handle is expected
EXPECT: reject
WHY: A value array cannot be implicitly converted to an array handle return type without handle syntax.
```angelscript
array<int>@ test(array<int> arr)
{
    return arr;
}
```

### CASE 16: Enum value returned from int function
EXPECT: accept
WHY: AngelScript implicitly converts enum values to primitive int in return statements.
```angelscript
enum Color { RED = 0, GREEN = 1 }

int test()
{
    return RED;
}
```

### CASE 17: Int literal returned from enum function
EXPECT: reject
WHY: Integer values cannot be implicitly converted to enum types without an explicit cast.
```angelscript
enum Color { RED = 0, GREEN = 1 }

Color test()
{
    return 0;
}
```

### CASE 18: Mismatched enum type returned from enum function
EXPECT: reject
WHY: Distinct enum types cannot be implicitly converted to one another.
```angelscript
enum State { OFF = 0, ON = 1 }
enum Color { RED = 0, GREEN = 1 }

Color test()
{
    return ON;
}
```

### CASE 19: Comparison expression returned from int function
EXPECT: reject
WHY: Comparison expressions yield a bool which cannot implicitly convert to an int return type.
```angelscript
int test(int a, int b)
{
    return a == b;
}
```

### CASE 20: Ternary expression with mixed branch types returned from float function
EXPECT: accept
WHY: The ternary operator promotes its int branch to float, matching the declared float return type.
```angelscript
float test(bool cond, int a, float b)
{
    return cond ? a : b;
}
```

### CASE 21: Assignment expression returned from int function
EXPECT: accept
WHY: An assignment expression yields a reference to the assigned variable which converts to the int return value.
```angelscript
int test()
{
    int val = 0;
    return val = 10;
}
```

### CASE 22: Empty return inside constructor
EXPECT: accept
WHY: An empty return statement is permitted inside a constructor to exit early.
```angelscript
class Widget
{
    Widget(bool skipInit)
    {
        if (skipInit) return;
    }
}
```

### CASE 23: Return with value inside constructor
EXPECT: reject
WHY: Constructors do not have a return type and cannot return a value.
```angelscript
class Widget
{
    Widget()
    {
        return 42;
    }
}
```

### CASE 24: Empty return inside void function
EXPECT: accept
WHY: An empty return statement without an expression is valid inside a void function.
```angelscript
void test(bool cond)
{
    if (cond)
        return;
}
```

### CASE 25: Lambda returning convertible numeric type for funcdef
EXPECT: accept
WHY: The lambda body returns an int which implicitly widens to the float return type specified by the funcdef.
```angelscript
funcdef float Callback(int x);

void setup()
{
    Callback@ cb = function(int x) { return x; };
}
```

### CASE 26: Lambda with empty return matching non-void funcdef
EXPECT: reject
WHY: A lambda must return a value compatible with the non-void return type of the matching funcdef.
```angelscript
funcdef int Compute();

void setup()
{
    Compute@ fn = function() { return; };
}
```

### CASE 27: Lambda returning incompatible object type for funcdef
EXPECT: reject
WHY: The lambda body returns an Entity instance which cannot be converted to the int return type expected by the funcdef.
```angelscript
class Entity {}
funcdef int Compute();

void setup()
{
    Compute@ fn = function() { Entity e; return e; };
}
```

### CASE 28: Non-void function with return only inside if branch
EXPECT: reject
WHY: Not all control paths return a value when the return statement is only inside an if block.
```angelscript
int test(bool cond)
{
    if (cond)
        return 1;
}
```

### CASE 29: Non-void function with returns in both if and else branches
EXPECT: accept
WHY: All control flow paths return a value because both branches of the if-else statement return.
```angelscript
int test(bool cond)
{
    if (cond)
        return 1;
    else
        return 2;
}
```

### CASE 30: Non-void function with return inside while(true) loop
EXPECT: accept
WHY: An infinite loop containing a return statement guarantees that all reachable execution paths return a value.
```angelscript
int test()
{
    while (true)
    {
        return 42;
    }
}
```
