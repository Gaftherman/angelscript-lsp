// Test 1: context-sensitive 'function' as funcdef name and as type
funcdef void function();
void test(function@ f)
{
    f();
}

// Test 2: context-sensitive keywords as class names (should be VALID - they are context-sensitive)
final class final {}
abstract class abstract {}

// Test 3: reserved keywords as class names (should be INVALID)
interface interface {}
class class {}

// Test 4: reserved keyword 'funcdef' used as a name
funcdef funcdef funcdef();
