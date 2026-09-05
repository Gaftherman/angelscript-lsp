### CASE 1: Switch on enum with all members
GROUP: switch
EXPECT: accept
WHY: AngelScript supports switching over enum values with each enum member acting as a constant case label.
```angelscript
enum State { A, B, C }
void test() {
    State s = A;
    switch (s) {
        case A: break;
        case B: break;
        case C: break;
    }
}
```

### CASE 2: Switch with duplicate case labels
GROUP: switch
EXPECT: reject
WHY: The compiler rejects switch statements containing duplicate case labels with the same constant value.
```angelscript
void test() {
    int x = 1;
    switch (x) {
        case 1: break;
        case 1: break;
    }
}
```

### CASE 3: Switch with non-constant case label
GROUP: switch
EXPECT: reject
WHY: Case labels in AngelScript must be compile-time constants, so non-constant variable expressions are rejected.
```angelscript
void test() {
    int x = 2;
    int y = 2;
    switch (x) {
        case y: break;
    }
}
```

### CASE 4: Switch declaring variable in case without block
GROUP: switch
EXPECT: reject
WHY: Declaring an initialized variable directly within a switch case without an enclosing block is prohibited because subsequent cases jump over initialization.
```angelscript
void test() {
    int x = 1;
    switch (x) {
        case 1:
            int y = 10;
            break;
        case 2:
            break;
    }
}
```

### CASE 5: Switch with default and break in every arm
GROUP: switch
EXPECT: accept
WHY: A switch with constant labels, a default clause, and break statements in all branches is valid syntax.
```angelscript
void test() {
    int x = 0;
    switch (x) {
        case 0:
            break;
        case 1:
            break;
        default:
            break;
    }
}
```

### CASE 6: Foreach over an array of integers
GROUP: loop
EXPECT: reject
WHY: AngelScript does not have a foreach keyword or range-based for loop syntax in the language specification.
```angelscript
void test() {
    array<int> list;
    foreach (int x in list) {
    }
}
```

### CASE 7: Foreach with loop variable used after loop ends
GROUP: loop
EXPECT: reject
WHY: The foreach construct is invalid syntax in AngelScript, and any loop-scoped variable is not visible outside its block.
```angelscript
void test() {
    array<int> list;
    foreach (int x in list) {
    }
    int y = x;
}
```

### CASE 8: For loop declaring two variables in initializer
GROUP: loop
EXPECT: accept
WHY: AngelScript allows declaring and initializing multiple variables of the same type in the for loop initializer statement.
```angelscript
void test() {
    int sum = 0;
    for (int i = 0, j = 10; i < j; i++) {
        sum += i;
    }
}
```

### CASE 9: While loop condition with assignment instead of comparison
GROUP: loop
EXPECT: reject
WHY: In AngelScript, assignment expressions do not evaluate to a value and cannot be used as conditions in control flow.
```angelscript
void test() {
    int x = 0;
    while (x = 1) {
        break;
    }
}
```

### CASE 10: Do-while body declares variable used in condition
GROUP: loop
EXPECT: reject
WHY: Variables declared inside the do-while body have block scope ending at the closing brace and are not accessible in the while condition.
```angelscript
void test() {
    do {
        int x = 0;
    } while (x == 0);
}
```

### CASE 11: Multiple variable declaration all used
GROUP: multi-decl
EXPECT: accept
WHY: Declaring and initializing multiple comma-separated variables of the same type in a single local statement is fully supported.
```angelscript
void test() {
    int a = 1, b = 2, c = 3;
    int sum = a + b + c;
}
```

### CASE 12: Multiple variable declaration with one unused
GROUP: multi-decl
EXPECT: accept
WHY: AngelScript successfully compiles functions with unused local variables declared in a multi-variable list.
```angelscript
void test() {
    int a = 1, b = 2, c = 3;
    int sum = a + c;
}
```

### CASE 13: Multiple variable declaration with duplicate name
GROUP: multi-decl
EXPECT: reject
WHY: Declaring two variables with identical names within the same scope is rejected as an identifier collision.
```angelscript
void test() {
    int a, a;
}
```

### CASE 14: Multiple variable declaration where second initializer uses first
GROUP: multi-decl
EXPECT: accept
WHY: The first variable is introduced into local scope immediately after its declarator and is valid to reference in subsequent initializers.
```angelscript
void test() {
    int a = 1, b = a + 2;
}
```

### CASE 15: Lambda declaring local variable and returning it
GROUP: lambda
EXPECT: accept
WHY: Anonymous functions can declare local variables within their body and return them conforming to the assigned funcdef.
```angelscript
funcdef int Compute(int);
void test() {
    Compute@ fn = function(int x) {
        int res = x * 2;
        return res;
    };
}
```

### CASE 16: Lambda containing a switch statement
GROUP: lambda
EXPECT: accept
WHY: Switch control flow statements with complete arms and returns are legal inside anonymous function bodies.
```angelscript
funcdef int Transform(int);
void test() {
    Transform@ fn = function(int x) {
        switch (x) {
            case 1: return 10;
            case 2: return 20;
            default: return 0;
        }
    };
}
```

### CASE 17: Lambda containing a for loop
GROUP: lambda
EXPECT: accept
WHY: For loops are permitted inside anonymous functions and can manipulate local variables before returning.
```angelscript
funcdef int SumUp(int);
void test() {
    SumUp@ fn = function(int n) {
        int total = 0;
        for (int i = 0; i < n; i++) {
            total += i;
        }
        return total;
    };
}
```

### CASE 18: Lambda declaring two variables with identical names
GROUP: lambda
EXPECT: reject
WHY: Declaring duplicate variable identifiers inside a lambda body causes a redeclaration compilation error.
```angelscript
funcdef void Action();
void test() {
    Action@ fn = function() {
        int val = 1;
        int val = 2;
    };
}
```

### CASE 19: Lambda return type mismatching assigned funcdef
GROUP: lambda
EXPECT: reject
WHY: An anonymous function with an empty return statement cannot be assigned to a funcdef expecting an integer return value.
```angelscript
funcdef int IntProducer();
void test() {
    IntProducer@ fn = function() {
        return;
    };
}
```

### CASE 20: Lambda capturing nothing and returning constant
GROUP: lambda
EXPECT: accept
WHY: A non-capturing anonymous function returning a constant literal matches the signature of the target funcdef.
```angelscript
funcdef int GetConstant();
void test() {
    GetConstant@ fn = function() {
        return 42;
    };
}
```

### CASE 21: Function call with too few arguments
GROUP: call-args
EXPECT: reject
WHY: Invoking a function with fewer arguments than parameters when no default values exist is a compilation error.
```angelscript
void target(int a, int b) {}
void test() {
    target(1);
}
```

### CASE 22: Function call with too many arguments
GROUP: call-args
EXPECT: reject
WHY: Passing more arguments than the declared parameter count of the matching function is rejected.
```angelscript
void target(int a) {}
void test() {
    target(1, 2);
}
```

### CASE 23: Function call with incompatible argument type
GROUP: call-args
EXPECT: reject
WHY: The compiler cannot implicitly convert a custom class instance to an expected primitive integer parameter.
```angelscript
class Dummy {}
void target(int a) {}
void test() {
    Dummy d;
    target(d);
}
```

### CASE 24: Function call with argument from another call
GROUP: call-args
EXPECT: accept
WHY: Passing the return value of one function as the argument to another compatible function is valid syntax.
```angelscript
int compute() { return 5; }
void target(int x) {}
void test() {
    target(compute());
}
```

### CASE 25: Assigning void function call result to variable
GROUP: call-return
EXPECT: reject
WHY: A function returning void yields no value, so attempting to assign its result to a variable is disallowed.
```angelscript
void doNothing() {}
void test() {
    int x = doNothing();
}
```

### CASE 26: Using call result where incompatible type is expected
GROUP: call-return
EXPECT: reject
WHY: Assigning an object handle returned from a call to a primitive integer variable is an incompatible type conversion.
```angelscript
class Node {}
Node@ getNode() { return null; }
void test() {
    int x = getNode();
}
```

### CASE 27: Call used as condition in if statement
GROUP: call-return
EXPECT: accept
WHY: A function call returning a boolean expression directly evaluates as a valid condition for an if statement.
```angelscript
bool isValid() { return true; }
void test() {
    if (isValid()) {
        int x = 1;
    }
}
```

### CASE 28: Nested function call feeding outer parameter
GROUP: call-return
EXPECT: accept
WHY: The return type of the inner call matches the parameter type of the outer call, enabling valid nested evaluation.
```angelscript
int stepOne(int a) { return a + 1; }
int stepTwo(int b) { return b * 2; }
void test() {
    int result = stepTwo(stepOne(5));
}
```
