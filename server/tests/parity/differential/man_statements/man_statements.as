// ==== case: stmt_var_decl_good ====
// @expect: accept
void f() {
    int a = 1;
}

// ==== case: stmt_var_decl_normal ====
// @expect: accept
void f() {
    int a = 10, b = 20;
    const float pi = 3.141592f;
    string s = "test";
    array<int> arr = {1, 2, 3};
    int c = a + b;
}

// ==== case: stmt_var_decl_bad ====
// @expect: reject
void f() {
    int x = 1;
    int x = 2;
}

// ==== case: stmt_expr_good ====
// @expect: accept
void f() {
    int x = 0;
    x++;
}

// ==== case: stmt_expr_normal ====
// @expect: accept
void helper(int val) {}
void f() {
    int x = 5;
    x += 10;
    helper(x);
    string s = "hello";
    s.resize(10);
}

// ==== case: stmt_expr_bad ====
// @expect: reject
// @range: loose
void f() {
    int x = 0;
    x++
}

// ==== case: stmt_block_good ====
// @expect: accept
void f() {
    {
        int x = 1;
    }
}

// ==== case: stmt_block_normal ====
// @expect: accept
void f() {
    int x = 10;
    {
        int y = x * 2;
        {
            int z = y + x;
            x = z;
        }
    }
}

// ==== case: stmt_block_bad ====
// @expect: reject
void f() {
    {
        int x = 1;
    }
    }
}

// ==== case: stmt_return_good ====
// @expect: accept
int f() {
    return 42;
}

// ==== case: stmt_return_normal ====
// @expect: accept
string classify(int val) {
    if (val < 0) {
        return "negative";
    } else if (val == 0) {
        return "zero";
    }
    return "positive";
}

// ==== case: stmt_return_bad ====
// @expect: reject
void f() {
    return 42;
}

// ==== case: stmt_empty_good ====
// @expect: accept
void f() {
    ;
}

// ==== case: stmt_empty_normal ====
// @expect: accept
void f() {
    int x = 1;
    ;
    x = x + 2;
    ;
}

// ==== case: stmt_empty_bad ====
// @expect: reject
void f(bool cond) {
    if (cond);
}

// ==== case: expr_assign_good ====
// @expect: accept
void f() {
    int a = 0;
    a = 5;
}

// ==== case: expr_assign_normal ====
// @expect: accept
void f() {
    int a = 0, b = 0, c = 0;
    a = b = c = 10;
    a += 5;
    b *= 2;
}

// ==== case: expr_assign_bad ====
// @expect: reject
void f() {
    5 = 10;
}

// ==== case: expr_arith_good ====
// @expect: accept
int f() {
    return 2 + 3;
}

// ==== case: expr_arith_normal ====
// @expect: accept
int compute(int a, int b) {
    int c = -(a + b) * 3;
    int d = (c / 2) % 5;
    return d;
}

// ==== case: expr_arith_bad ====
// @expect: reject
void f() {
    string a = "hello";
    string b = "world";
    string c = a - b;
}

// ==== case: expr_bitwise_good ====
// @expect: accept
uint f(uint a) {
    return ~a;
}

// ==== case: expr_bitwise_normal ====
// @expect: accept
uint mask(uint val, uint shift) {
    uint a = (val & 0xFF) | ((val ^ 0xAA) << shift);
    uint b = (a >> 2) >>> 1;
    return b;
}

// ==== case: expr_bitwise_bad ====
// @expect: reject
void f() {
    float a = 1.0f;
    float b = a & 2.0f;
}

// ==== case: expr_logic_good ====
// @expect: accept
bool f(bool a, bool b) {
    return a and b;
}

// ==== case: expr_logic_normal ====
// @expect: accept
bool test(bool a, bool b, bool c) {
    return (a and b) or (not c) or (a xor b);
}

// ==== case: expr_logic_bad ====
// @expect: reject
void f() {
    string s = "test";
    bool b = s and true;
}

// ==== case: expr_relational_good ====
// @expect: accept
bool f(int a, int b) {
    return a < b;
}

// ==== case: expr_relational_normal ====
// @expect: accept
bool check(int x, double y) {
    return (x >= 0) && (x <= 100) && (y != 0.0) && (x == int(y));
}

// ==== case: expr_relational_bad ====
// @expect: reject
void f() {
    bool b = 10 < "string";
}

// ==== case: expr_identity_good ====
// @expect: accept
class Obj {}
bool f(Obj@ a) {
    return a is null;
}

// ==== case: expr_identity_normal ====
// @expect: accept
class Node {}
bool same(Node@ a, Node@ b) {
    if (a is null or b is null) return false;
    return a is b or !(a !is b);
}

// ==== case: expr_identity_bad ====
// @expect: reject
void f() {
    int a = 1;
    int b = 2;
    bool res = (a is b);
}

// ==== case: expr_call_out_good ====
// @expect: accept
void getVal(int &out val) { val = 42; }
void f() {
    int x;
    getVal(x);
}

// ==== case: expr_call_out_normal ====
// @expect: accept
void getTwo(int &out a, string &out b) { a = 1; b = "ok"; }
void f() {
    int x;
    getTwo(x, void);
}

// ==== case: expr_call_out_bad ====
// @expect: reject
void getVal(int &out val) { val = 42; }
void f() {
    getVal(123);
}

// ==== case: expr_call_named_good ====
// @expect: accept
void config(int width = 800, int height = 600) {}
void f() {
    config(height: 1080);
}

// ==== case: expr_call_named_normal ====
// @expect: accept
void setProps(int x = 0, int y = 0, string title = "App") {}
void f() {
    setProps(title: "Window", y: 200, x: 100);
}

// ==== case: expr_call_named_bad ====
// @expect: reject
void setProps(int x = 0, int y = 0, int z = 0) {}
void f() {
    setProps(y: 1, 2);
}

// ==== case: expr_index_good ====
// @expect: accept
int f(array<int>@ arr) {
    return arr[0];
}

// ==== case: expr_index_normal ====
// @expect: accept
void f() {
    array<string> arr = {"apple", "banana"};
    arr[0] = "cherry";
    string s = arr[1];
}

// ==== case: expr_index_bad ====
// @expect: reject
void f() {
    int n = 42;
    int x = n[0];
}

// ==== case: expr_member_good ====
// @expect: accept
class Point { int x; int y; }
int f(Point@ p) {
    return p.x;
}

// ==== case: expr_member_normal ====
// @expect: accept
class Account {
    string name;
    int balance;
    void deposit(int amt) { balance += amt; }
}
void f() {
    Account acc;
    acc.name = "Savings";
    acc.deposit(100);
}

// ==== case: expr_member_bad ====
// @expect: reject
class Point { int x; }
void f(Point@ p) {
    int y = p.y;
}

// ==== case: expr_cast_good ====
// @expect: accept
int f(float val) {
    return int(val);
}

// ==== case: expr_cast_normal ====
// @expect: accept
interface Animal {}
class Dog : Animal {}
void f(Animal@ a) {
    Dog@ d = cast<Dog>(a);
}

// ==== case: expr_cast_bad ====
// @expect: reject
class Cat {}
void f() {
    int n = 5;
    Cat@ c = cast<Cat>(n);
}

// ==== case: enum_decl_good ====
// @expect: accept
enum Status { Pending, Active, Done }

// ==== case: enum_decl_normal ====
// @expect: accept
enum Priority {
    Low = 1,
    Medium = Low * 2,
    High = Medium + 5
}

// ==== case: enum_decl_bad ====
// @expect: reject
enum Status {
    Active,
    Active
}

// ==== case: enum_typed_good ====
// @expect: accept
enum ByteFlag : uint8 {
    Bit0 = 1
}

// ==== case: enum_typed_normal ====
// @expect: accept
enum PacketType : int16 {
    Ping = 100,
    Pong = 101,
    Data = 200
}
void send(PacketType p) {}
void f() {
    send(PacketType::Ping);
}

// ==== case: enum_typed_bad ====
// @expect: reject
enum BadEnum : string {
    Item1 = 1
}

// ==== case: enum_usage_good ====
// @expect: accept
enum Mode { Fast, Slow }
void f() {
    Mode m = Mode::Fast;
}

// ==== case: enum_usage_normal ====
// @expect: accept
enum Direction { North, East, South, West }
Direction opposite(Direction d) {
    if (d == Direction::North) return Direction::South;
    if (d == Direction::East) return Direction::West;
    if (d == Direction::South) return Direction::North;
    return Direction::East;
}

// ==== case: enum_usage_bad ====
// @expect: reject
enum Mode { Fast, Slow }
void f() {
    Mode m = "Fast";
}

// ==== case: ns_decl_good ====
// @expect: accept
namespace Core {
    int version = 1;
}

// ==== case: ns_decl_normal ====
// @expect: accept
namespace Game {
    namespace Physics {
        const float Gravity = 9.8f;
        float fallSpeed(float t) { return Gravity * t; }
    }
}
float test() {
    return Game::Physics::fallSpeed(2.0f);
}

// ==== case: ns_decl_bad ====
// @expect: reject
namespace 123 {
    void f() {}
}

// ==== case: ns_using_global_good ====
// @expect: accept
namespace Lib {
    void util() {}
}
using namespace Lib;
void f() {
    util();
}

// ==== case: ns_using_global_normal ====
// @expect: accept
namespace Math {
    int add(int a, int b) { return a + b; }
}
namespace Str {
    string concat(string a, string b) { return a + b; }
}
using namespace Math;
using namespace Str;
void test() {
    int sum = add(1, 2);
    string msg = concat("a", "b");
}

// ==== case: ns_using_global_bad ====
// @expect: reject
using UnknownNS;
void f() {}

// ==== case: ns_using_block_good ====
// @expect: accept
namespace Tools {
    int run() { return 1; }
}
void f() {
    {
        using namespace Tools;
        int x = run();
    }
}

// ==== case: ns_using_block_normal ====
// @expect: accept
namespace Data {
    int value = 42;
}
int f() {
    int outVal = 0;
    {
        using namespace Data;
        outVal = value;
    }
    return outVal + Data::value;
}

// ==== case: ns_using_block_bad ====
// @expect: reject
namespace Data {
    int secretValue = 100;
}
void f() {
    {
        using namespace Data;
    }
    int x = secretValue;
}

// ==== case: global_var_decl_good ====
// @expect: accept
int g_counter = 0;

// ==== case: global_var_decl_normal ====
// @expect: accept
int g_total = 100;
string g_appName = "AngelApp";
void incrementTotal(int amount) {
    g_total += amount;
}

// ==== case: global_var_decl_bad ====
// @expect: reject
int g_val = 1;
int g_val = 2;

// ==== case: global_const_good ====
// @expect: accept
const int MAX_USERS = 50;

// ==== case: global_const_normal ====
// @expect: accept
const double PI = 3.1415926535;
const string DEFAULT_PATH = "/usr/local";
double circleArea(double r) {
    return PI * r * r;
}

// ==== case: global_const_bad ====
// @expect: reject
const int LIMIT = 10;
void f() {
    LIMIT = 20;
}

// ==== case: switch_int_good ====
// @expect: accept
int f(int x) {
    switch (x) {
        case 0: return 0;
        default: return -1;
    }
}

// ==== case: switch_int_normal ====
// @expect: accept
const int TWO = 2;
int evaluate(int val) {
    int result = 0;
    switch (val) {
        case 0:
        case 1:
            result = 10;
            break;
        case TWO:
            result = 20;
            break;
        default:
            result = 99;
            break;
    }
    return result;
}

// ==== case: switch_int_bad ====
// @expect: reject
void f(int x) {
    switch (x) {
        case 1: break;
        case 1: break;
    }
}

// ==== case: switch_enum_good ====
// @expect: accept
enum State { S0, S1 }
int f(State s) {
    switch (s) {
        case State::S0: return 0;
        default: return 1;
    }
}

// ==== case: switch_enum_normal ====
// @expect: accept
enum Action { Start, Stop, Pause }
string describe(Action a) {
    switch (a) {
        case Action::Start:
            return "starting";
        case Action::Stop:
            return "stopping";
        case Action::Pause:
            return "pausing";
    }
    return "unknown";
}

// ==== case: switch_enum_bad ====
// @expect: reject
int getConst() { return 1; }
void f(int x) {
    switch (x) {
        case getConst(): break;
    }
}

// ==== case: loop_while_good ====
// @expect: accept
void f() {
    while (false) {}
}

// ==== case: loop_while_normal ====
// @expect: accept
int sum(int n) {
    int i = 0, total = 0;
    while (i < n) {
        total += i;
        i++;
    }
    return total;
}

// ==== case: loop_while_bad ====
// @expect: reject
void f() {
    string s = "test";
    while (s) {}
}

// ==== case: loop_dowhile_good ====
// @expect: accept
void f() {
    do {} while (false);
}

// ==== case: loop_dowhile_normal ====
// @expect: accept
int factorial(int n) {
    int res = 1;
    int i = n;
    do {
        res *= i;
        i--;
    } while (i > 1);
    return res;
}

// ==== case: loop_dowhile_bad ====
// @expect: reject
// @range: loose
void f() {
    do {} while (false)
}

// ==== case: loop_for_good ====
// @expect: accept
void f() {
    for (int i = 0; i < 5; i++) {}
}

// ==== case: loop_for_normal ====
// @expect: accept
int f(int limit) {
    int count = 0;
    for (int i = 0, j = limit; i < j; i++, j--) {
        count += (j - i);
    }
    for (;;) {
        if (count > 100) break;
        count++;
    }
    return count;
}

// ==== case: loop_for_bad ====
// @expect: reject
void f() {
    for (int i = 0, float f = 1.0f; i < 5; i++) {}
}

// ==== case: loop_break_continue_good ====
// @expect: accept
void f() {
    while (true) {
        break;
    }
}

// ==== case: loop_break_continue_normal ====
// @expect: accept
int process(int rows, int cols) {
    int total = 0;
    for (int r = 0; r < rows; r++) {
        if (r % 2 == 0) continue;
        for (int c = 0; c < cols; c++) {
            if (c == 5) break;
            total += (r * cols + c);
        }
    }
    return total;
}

// ==== case: loop_break_continue_bad ====
// @expect: reject
void f() {
    continue;
}

// ==== case: foreach_array_good ====
// @expect: accept
void f() {
    array<int> arr = {1, 2, 3};
    foreach(int x : arr) {}
}

// ==== case: foreach_array_normal ====
// @expect: accept
int sumIndexed(array<int>@ arr) {
    int total = 0;
    foreach(auto val, auto idx : arr) {
        total += (val * (idx + 1));
    }
    return total;
}

// ==== case: foreach_array_bad ====
// @expect: reject
void f() {
    int notAContainer = 42;
    foreach(auto x : notAContainer) {}
}

// ==== case: try_catch_good ====
// @expect: accept
void f() {
    try {} catch {}
}

// ==== case: try_catch_normal ====
// @expect: accept
int safeDiv(int a, int b) {
    try {
        return a / b;
    } catch {
        return 0;
    }
}

// ==== case: try_catch_bad ====
// @expect: reject
void f() {
    try {
        int x = 1;
    } catch (int e) {
    }
}

// ==== case: ternary_rvalue_good ====
// @expect: accept
int f(bool cond) {
    return cond ? 1 : 2;
}

// ==== case: ternary_rvalue_normal ====
// @expect: accept
string scoreGrade(int score) {
    return score >= 90 ? "A" : (score >= 80 ? "B" : (score >= 70 ? "C" : "F"));
}

// ==== case: ternary_rvalue_bad ====
// @expect: reject
void f(bool cond) {
    int x = cond ? 42 : "invalid";
}

// ==== case: ternary_lvalue_good ====
// @expect: accept
void f(bool cond) {
    int a = 0, b = 0;
    (cond ? a : b) = 10;
}

// ==== case: ternary_lvalue_normal ====
// @expect: accept
void selectMax(int a, int b, int val) {
    int x = a, y = b;
    (x > y ? x : y) = val;
}

// ==== case: ternary_lvalue_bad ====
// @expect: reject
void f(bool cond) {
    int a = 0;
    (cond ? a : 10) = 42;
}

// ==== case: scope_shadow_good ====
// @expect: accept
void f() {
    int x = 1;
    {
        int x = 2;
    }
}

// ==== case: scope_shadow_normal ====
// @expect: accept
int compute() {
    int val = 10;
    {
        float val = 3.5f;
        int inner = int(val * 2);
    }
    return val;
}

// ==== case: scope_shadow_bad ====
// @expect: reject
void f() {
    {
        int innerVar = 42;
    }
    int x = innerVar;
}

// ==== case: scope_resolution_good ====
// @expect: accept
int g_val = 100;
int f() {
    int g_val = 5;
    return ::g_val;
}

// ==== case: scope_resolution_normal ====
// @expect: accept
int val = 1;
namespace Outer {
    int val = 2;
    namespace Inner {
        int val = 3;
        int sum() {
            return val + Outer::val + ::val;
        }
    }
}

// ==== case: scope_resolution_bad ====
// @expect: reject
void f() {
    int x = ::nonExistentGlobalVariable;
}

// ==== case: import_func_good ====
// @expect: accept
import void extFunc() from "ForeignModule";
void f() {
    extFunc();
}

// ==== case: import_func_normal ====
// @expect: accept
import int calculateHash(const string&in data) from "SecurityModule";
import bool verifyToken(int id, const string&in token) from "SecurityModule";
bool auth(const string&in name, const string&in tok) {
    int h = calculateHash(name);
    return verifyToken(h, tok);
}

// ==== case: import_func_bad ====
// @expect: reject
import void brokenFunc();

