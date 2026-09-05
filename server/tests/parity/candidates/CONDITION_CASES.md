### CASE 1: Bool variable as condition
EXPECT: accept
WHY: A bool variable is a valid boolean expression for an if condition.
```angelscript
void test() {
    bool flag = true;
    if (flag) {}
}
```

### CASE 2: Int variable as condition
EXPECT: reject
WHY: AngelScript requires a boolean condition and does not implicitly convert int to bool.
```angelscript
void test() {
    int x = 1;
    if (x) {}
}
```

### CASE 3: Comparison of two ints
EXPECT: accept
WHY: Relational comparison between two integers produces a bool result.
```angelscript
void test() {
    int a = 1;
    int b = 2;
    if (a < b) {}
}
```

### CASE 4: Bitwise expression as condition
EXPECT: reject
WHY: Bitwise AND yields an int, which cannot implicitly convert to bool.
```angelscript
void test() {
    int flags = 3;
    if (flags & 1) {}
}
```

### CASE 5: Logical NOT on an int
EXPECT: reject
WHY: The logical NOT operator requires a boolean operand and is not defined for int.
```angelscript
void test() {
    int x = 0;
    if (!x) {}
}
```

### CASE 6: Logical NOT on a bool
EXPECT: accept
WHY: The logical NOT operator on a bool evaluates to a bool.
```angelscript
void test() {
    bool ready = false;
    if (!ready) {}
}
```

### CASE 7: Logical AND of two bools
EXPECT: accept
WHY: Logical AND between two bool operands evaluates to a bool.
```angelscript
void test() {
    bool a = true;
    bool b = false;
    if (a && b) {}
}
```

### CASE 8: Logical AND with bool and int
EXPECT: reject
WHY: Both operands of logical AND must be of boolean type.
```angelscript
void test() {
    bool a = true;
    int count = 5;
    if (a && count) {}
}
```

### CASE 9: Function returning bool as condition
EXPECT: accept
WHY: A call to a function returning bool satisfies the boolean condition requirement.
```angelscript
bool isReady() {
    return true;
}
void test() {
    if (isReady()) {}
}
```

### CASE 10: Function returning int as condition
EXPECT: reject
WHY: A call returning int produces an integer expression that cannot convert to bool.
```angelscript
int getCount() {
    return 1;
}
void test() {
    if (getCount()) {}
}
```

### CASE 11: Ternary condition using int
EXPECT: reject
WHY: The condition expression of a ternary operator must be of boolean type.
```angelscript
void test() {
    int cond = 1;
    int result = cond ? 10 : 20;
}
```

### CASE 12: Ternary condition using comparison
EXPECT: accept
WHY: The comparison in the ternary condition expression yields a bool.
```angelscript
void test() {
    int a = 5;
    int result = (a > 0) ? 10 : 20;
}
```

### CASE 13: While loop with true and break
EXPECT: accept
WHY: The boolean literal true is a valid condition for a while loop.
```angelscript
void test() {
    while (true) {
        break;
    }
}
```

### CASE 14: For loop with comparison condition
EXPECT: accept
WHY: The middle expression of the for loop is a comparison evaluating to bool.
```angelscript
void test() {
    for (int i = 0; i < 10; i++) {}
}
```

### CASE 15: For loop with int condition
EXPECT: reject
WHY: The middle condition expression of a for loop must evaluate to a boolean type.
```angelscript
void test() {
    for (int i = 0; i; i++) {}
}
```

### CASE 16: Do-while loop on a bool
EXPECT: accept
WHY: The condition expression of a do-while loop is a valid bool variable.
```angelscript
void test() {
    bool flag = false;
    do {} while (flag);
}
```

### CASE 17: Class instance without conversion as condition
EXPECT: reject
WHY: A class instance cannot be converted to bool when no conversion operator is defined.
```angelscript
class Item {}
void test() {
    Item item;
    if (item) {}
}
```

### CASE 18: Handle compared with is null
EXPECT: accept
WHY: The identity comparison operator 'is' evaluates to a bool.
```angelscript
class Item {}
void test() {
    Item@ handle = null;
    if (handle is null) {}
}
```

### CASE 19: Bare handle as condition
EXPECT: reject
WHY: Object handles do not implicitly convert to bool and require explicit comparison.
```angelscript
class Item {}
void test() {
    Item@ handle = null;
    if (handle) {}
}
```

### CASE 20: Enum value as condition
EXPECT: reject
WHY: Enums are integer types and do not implicitly convert to bool.
```angelscript
enum State { Stopped, Running }
void test() {
    State s = Running;
    if (s) {}
}
```

### CASE 21: Assignment expression as condition
EXPECT: reject
WHY: An assignment expression yields an int reference which cannot convert to bool.
```angelscript
void test() {
    int x = 0;
    if (x = 1) {}
}
```

### CASE 22: Parenthesised comparison
EXPECT: accept
WHY: The parenthesised arithmetic expression is compared with zero, yielding a bool.
```angelscript
void test() {
    int a = 2;
    int b = 3;
    if ((a + b) > 0) {}
}
```
