### CASE 1: Undeclared type and missing return value in function
GROUP: multi-error
EXPECT: reject
WHY: Rejects due to undeclared type 'UnknownType' and missing return value in non-void function 'compute'.
FILES:
--- main.as
```angelscript
int compute() {
    UnknownType item;
    return;
}
```

### CASE 2: Three functions with distinct independent errors
GROUP: multi-error
EXPECT: reject
WHY: Rejects due to undeclared identifier in f1, invalid type conversion in f2, and missing return value in f3.
FILES:
--- main.as
```angelscript
void f1() {
    undeclaredVar = 10;
}

void f2() {
    int a = true;
}

int f3() {
    return;
}
```

### CASE 3: Error inside class definition and error in trailing function
GROUP: multi-error
EXPECT: reject
WHY: Rejects due to undeclared member type inside class Widget and nonexistent function call in main.
FILES:
--- main.as
```angelscript
class Widget {
    NoSuchType member;
    void doWork() {}
}

void main() {
    callUndefinedFunction();
}
```

### CASE 4: Errors on the first and last lines of document
GROUP: multi-error
EXPECT: reject
WHY: Rejects due to undefined global type on line 1 and incomplete assignment syntax on the final line.
FILES:
--- main.as
```angelscript
UndefinedType globalVal;

void helper() {
    int x = 10;
    x += 5;
}

void finalize() {
    int y = ;
}
```

### CASE 5: Top-level error combined with deeply nested block error
GROUP: multi-error
EXPECT: reject
WHY: Rejects due to undeclared top-level variable type and undeclared identifier inside triply-nested block.
FILES:
--- main.as
```angelscript
NonexistentType rootHeader;

void run() {
    if (true) {
        while (true) {
            for (int i = 0; i < 5; i++) {
                deeplyNestedVar = 42;
            }
        }
    }
}
```

### CASE 6: Two independent syntax and semantic errors on the same line
GROUP: multi-error
EXPECT: reject
WHY: Rejects due to both an undeclared type and an undeclared identifier in a single variable initialization statement.
FILES:
--- main.as
```angelscript
void test() {
    MissingType val = nonexistentSource;
}
```

### CASE 7: Identical function signature in reopened namespace across files
GROUP: namespace
EXPECT: reject
WHY: Rejects because function 'void NS::worker()' is defined with identical signature in both files.
FILES:
--- main.as
```angelscript
#include "other.as"

namespace NS {
    void worker() {}
}
```
--- other.as
```angelscript
namespace NS {
    void worker() {}
}
```

### CASE 8: Function overload in reopened namespace across files
GROUP: namespace
EXPECT: accept
WHY: Compiles because AngelScript allows function overloading across reopened namespaces in separate files.
FILES:
--- main.as
```angelscript
#include "other.as"

namespace MathUtil {
    float compute(float x) { return x * 2.0f; }
}

void main() {
    int a = MathUtil::compute(5);
    float b = MathUtil::compute(5.0f);
}
```
--- other.as
```angelscript
namespace MathUtil {
    int compute(int x) { return x * 2; }
}
```

### CASE 9: Duplicate class declaration in reopened namespace across files
GROUP: namespace
EXPECT: reject
WHY: Rejects because class 'NS::Packet' is defined twice within the same namespace across files.
FILES:
--- main.as
```angelscript
#include "other.as"

namespace NS {
    class Packet {
        float timestamp;
    }
}
```
--- other.as
```angelscript
namespace NS {
    class Packet {
        int id;
    }
}
```

### CASE 10: Reopening namespace with distinct members across files
GROUP: namespace
EXPECT: accept
WHY: Compiles because namespaces can be reopened across files to declare distinct non-conflicting members.
FILES:
--- main.as
```angelscript
#include "other.as"

namespace Shared {
    int getBeta() { return 20; }
}

void main() {
    int total = Shared::getAlpha() + Shared::getBeta();
}
```
--- other.as
```angelscript
namespace Shared {
    int getAlpha() { return 10; }
}
```

### CASE 11: Nested namespace reopened across files
GROUP: namespace
EXPECT: accept
WHY: Compiles because nested namespaces can be reopened across files and access sibling symbols within scope.
FILES:
--- main.as
```angelscript
#include "other.as"

namespace Outer {
    namespace Inner {
        int generate() {
            return seed() + 1;
        }
    }
}

void main() {
    Outer::Inner::generate();
}
```
--- other.as
```angelscript
namespace Outer {
    namespace Inner {
        int seed() { return 42; }
    }
}
```

### CASE 12: Remote namespace symbol used unqualified without using directive
GROUP: namespace
EXPECT: reject
WHY: Rejects because 'service()' declared in namespace Core cannot be called unqualified without a using directive.
FILES:
--- main.as
```angelscript
#include "other.as"

void main() {
    service();
}
```
--- other.as
```angelscript
namespace Core {
    void service() {}
}
```

### CASE 13: Deeply nested blocks five levels deep
GROUP: braces
EXPECT: accept
WHY: Compiles successfully with five levels of correctly balanced nested compound blocks.
FILES:
--- main.as
```angelscript
void run() {
    int a = 1;
    {
        int b = a + 1;
        {
            int c = b + 1;
            {
                int d = c + 1;
                {
                    int e = d + 1;
                }
            }
        }
    }
}
```

### CASE 14: Missing closing brace at end of function
GROUP: braces
EXPECT: reject
WHY: Rejects due to unexpected end of file while parsing function body missing closing brace.
FILES:
--- main.as
```angelscript
void execute() {
    if (true) {
        int x = 10;
    }
```

### CASE 15: Extra closing brace following function definition
GROUP: braces
EXPECT: reject
WHY: Rejects due to unexpected closing brace token at global scope following function definition.
FILES:
--- main.as
```angelscript
void execute() {
    int x = 5;
}
}
```

### CASE 16: Missing semicolon inside deeply nested balanced blocks
GROUP: braces
EXPECT: reject
WHY: Rejects due to missing semicolon on statement three levels deep despite fully balanced braces.
FILES:
--- main.as
```angelscript
void execute() {
    if (true) {
        while (true) {
            {
                int val = 10
                break;
            }
        }
    }
}
```

### CASE 17: Fully braced if-else-if-else ladder
GROUP: braces
EXPECT: accept
WHY: Compiles correctly with explicit brace blocks around every branch of an if-else ladder.
FILES:
--- main.as
```angelscript
int evaluate(int score) {
    if (score > 90) {
        return 1;
    } else if (score > 75) {
        return 2;
    } else if (score > 50) {
        return 3;
    } else {
        return 0;
    }
}
```

### CASE 18: Switch statement with scoped compound braces in case clauses
GROUP: braces
EXPECT: accept
WHY: Compiles correctly with scoped compound statement braces enclosing case and default clauses.
FILES:
--- main.as
```angelscript
int handle(int code) {
    int result = 0;
    switch (code) {
        case 1: {
            int temp = 10;
            result = temp * 2;
            break;
        }
        case 2: {
            int temp = 20;
            result = temp + 5;
            break;
        }
        default: {
            result = -1;
            break;
        }
    }
    return result;
}
```

### CASE 19: Integer division assigned to float target
GROUP: engine-property
EXPECT: accept
WHY: Compiles under default asEP_COMPILER_WARNINGS (1), whereas setting warnings-as-errors (2) would reject the int-to-float division warning.
FILES:
--- main.as
```angelscript
void main() {
    float val = 1 / 2;
}
```

### CASE 20: Named argument call syntax using equals assignment
GROUP: engine-property
EXPECT: reject
WHY: Rejects under default asEP_ALTER_SYNTAX (0) where named args require colon syntax; enabling asEP_ALTER_SYNTAX would accept equals.
FILES:
--- main.as
```angelscript
void setup(int width, int height) {}

void main() {
    setup(width = 800, height = 600);
}
```

### CASE 21: Object evaluated in if condition via opImplConv
GROUP: engine-property
EXPECT: reject
WHY: Rejects under default asEP_ALTER_SYNTAX (0) because opImplConv to bool is not implicitly invoked in if conditions; asEP_ALTER_SYNTAX would accept it.
FILES:
--- main.as
```angelscript
class Flag {
    bool opImplConv() const {
        return true;
    }
}

void main() {
    Flag obj;
    if (obj) {}
}
```

### CASE 22: Global variable declaration at script module scope
GROUP: engine-property
EXPECT: accept
WHY: Compiles under default asEP_DISALLOW_GLOBAL_VARS (false); setting asEP_DISALLOW_GLOBAL_VARS to true would reject global variables.
FILES:
--- main.as
```angelscript
int g_counter = 100;

void main() {
    g_counter++;
}
```

### CASE 23: Value assignment between object handles of reference type
GROUP: engine-property
EXPECT: accept
WHY: Compiles under default asEP_DISALLOW_VALUE_ASSIGN_FOR_REF_TYPE (false); setting it to true would reject value copy between handles.
FILES:
--- main.as
```angelscript
class Node {
    int value;
}

void main() {
    Node@ a = Node();
    Node@ b = Node();
    a = b;
}
```

### CASE 24: Foreach loop statement syntax
GROUP: engine-property
EXPECT: reject
WHY: Rejects under default asEP_ALTER_SYNTAX (0) because foreach is not a standard keyword; enabling asEP_ALTER_SYNTAX would accept it.
FILES:
--- main.as
```angelscript
void main() {
    foreach (int x in collection) {}
}
```
