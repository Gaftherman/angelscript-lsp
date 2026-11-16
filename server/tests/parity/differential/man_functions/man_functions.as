// ==== case: func_decl_good ====
// @expect: accept
int add(int a, int b) { return a + b; }
void main() { int x = add(1, 2); }

// ==== case: func_decl_normal ====
// @expect: accept
int clamp(int val, int low, int high) {
    if (val < low) return low;
    if (val > high) return high;
    return val;
}
void main() {
    int c = clamp(15, 0, 10);
}

// ==== case: func_decl_bad ====
// @expect: reject
void f(int a, int a) {}
void main() {}

// ==== case: func_val_param_good ====
// @expect: accept
void setVal(int x) { x = 10; }
void main() { int a = 5; setVal(a); }

// ==== case: func_val_param_normal ====
// @expect: accept
int scaleAndOffset(int val, int scale, int offset) {
    val = val * scale + offset;
    return val;
}
void main() {
    int res = scaleAndOffset(5, 2, 3);
}

// ==== case: func_val_param_bad ====
// @expect: reject
void f(int x) {}
void main() { f("hello"); }

// ==== case: func_recursion_good ====
// @expect: accept
int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}
void main() { int f = factorial(4); }

// ==== case: func_recursion_normal ====
// @expect: accept
int sumArray(const array<int> &in arr, uint idx) {
    if (idx >= arr.length()) return 0;
    return arr[idx] + sumArray(arr, idx + 1);
}
void main() {
    array<int> data = {1, 2, 3};
    int total = sumArray(data, 0);
}

// ==== case: func_recursion_bad ====
// @expect: reject
int countdown(int n) {
    if (n <= 0) return 0;
    return countdown(n - 1, 0);
}
void main() { countdown(3); }

// ==== case: func_order_good ====
// @expect: accept
void main() { helper(); }
void helper() {}

// ==== case: func_order_normal ====
// @expect: accept
bool isEven(int n) {
    if (n == 0) return true;
    return isOdd(n - 1);
}
bool isOdd(int n) {
    if (n == 0) return false;
    return isEven(n - 1);
}
void main() { bool e = isEven(4); }

// ==== case: func_order_bad ====
// @expect: reject
void main() { notFoundFunction(); }

// ==== case: func_handle_ret_good ====
// @expect: accept
class Node {}
Node@ createNode() { return Node(); }
void main() { Node@ n = createNode(); }

// ==== case: func_handle_ret_normal ====
// @expect: accept
class Item { int id; Item(int i) { id = i; } }
Item@ findItem(int id) {
    if (id > 0) return Item(id);
    return null;
}
void main() { Item@ it = findItem(5); }

// ==== case: func_handle_ret_bad ====
// @expect: reject
class Node {}
Node@ createNode() { return 42; }
void main() {}

// ==== case: overload_arity_good ====
// @expect: accept
int compute(int a) { return a; }
int compute(int a, int b) { return a + b; }
void main() { int x = compute(1); int y = compute(1, 2); }

// ==== case: overload_arity_normal ====
// @expect: accept
string formatMsg(string msg) { return msg; }
string formatMsg(string msg, string prefix) { return prefix + ": " + msg; }
string formatMsg(string msg, string prefix, string suffix) { return prefix + ": " + msg + " " + suffix; }
void main() {
    string a = formatMsg("hi");
    string b = formatMsg("hi", "INFO");
    string c = formatMsg("hi", "INFO", "DONE");
}

// ==== case: overload_arity_bad ====
// @expect: reject
int f(int a) { return a; }
int f(int a, int b) { return a + b; }
void main() { int x = f(1, 2, 3); }

// ==== case: overload_type_good ====
// @expect: accept
string describe(int x) { return "int"; }
string describe(float x) { return "float"; }
void main() { string s1 = describe(1); string s2 = describe(1.0f); }

// ==== case: overload_type_normal ====
// @expect: accept
int process(int val) { return val * 2; }
int process(const string &in val) { return val.length(); }
int process(const array<int> &in val) { return int(val.length()); }
void main() {
    array<int> arr = {1, 2};
    int a = process(10);
    int b = process("hello");
    int c = process(arr);
}

// ==== case: overload_type_bad ====
// @expect: reject
void choose(int64 a, double b) {}
void choose(double a, int64 b) {}
void main() { choose(1, 1); }

// ==== case: overload_ret_only_good ====
// @expect: accept
int getVal(int key) { return key; }
string getVal(string key) { return key; }
void main() { int x = getVal(1); string s = getVal("a"); }

// ==== case: overload_ret_only_normal ====
// @expect: accept
class Resolver {
    int resolve(int id) { return id; }
    string resolve(string name) { return name; }
}
void main() {
    Resolver r;
    int i = r.resolve(42);
    string s = r.resolve("test");
}

// ==== case: overload_ret_only_bad ====
// @expect: reject
int getData(int id) { return id; }
string getData(int id) { return "id"; }
void main() {}

// ==== case: overload_const_method_good ====
// @expect: accept
class Buffer {
    int val;
    int get() { return val; }
    int get() const { return val; }
}
void main() {
    Buffer b;
    const Buffer@ cb = b;
    int x = b.get();
    int y = cb.get();
}

// ==== case: overload_const_method_normal ====
// @expect: accept
class Container {
    int item;
    int& get() { return item; }
    const int& get() const { return item; }
}
void main() {
    Container c;
    c.get() = 10;
    const Container@ cc = c;
    int val = cc.get();
}

// ==== case: overload_const_method_bad ====
// @expect: reject
class Writer {
    void write() {}
}
void main() {
    const Writer@ w = Writer();
    w.write();
}

// ==== case: defarg_single_good ====
// @expect: accept
int inc(int x, int step = 1) { return x + step; }
void main() { int a = inc(5); int b = inc(5, 2); }

// ==== case: defarg_single_normal ====
// @expect: accept
string greet(string name, string greeting = "Hello") {
    return greeting + ", " + name;
}
void main() {
    string g1 = greet("Alice");
    string g2 = greet("Bob", "Hi");
}

// ==== case: defarg_single_bad ====
// @expect: reject
int add(int a = 1, int b) { return a + b; }
void main() {}

// ==== case: defarg_multi_good ====
// @expect: accept
int config(int a = 1, int b = 2, int c = 3) { return a + b + c; }
void main() { int x = config(); int y = config(10); int z = config(10, 20); }

// ==== case: defarg_multi_normal ====
// @expect: accept
int boxVolume(int w, int h = 10, int d = 5) { return w * h * d; }
void main() {
    int v1 = boxVolume(2);
    int v2 = boxVolume(2, 3);
    int v3 = boxVolume(2, 3, 4);
}

// ==== case: defarg_multi_bad ====
// @expect: reject
void setBox(int w = 1, int h, int d = 3) {}
void main() {}

// ==== case: defarg_expr_good ====
// @expect: accept
int g_default = 42;
int getVal(int x = g_default) { return x; }
void main() { int v = getVal(); }

// ==== case: defarg_expr_normal ====
// @expect: accept
int defaultStep() { return 5; }
int stepUp(int base, int step = defaultStep() * 2) { return base + step; }
void main() {
    int s = stepUp(10);
}

// ==== case: defarg_expr_bad ====
// @expect: reject
void f(int x = unknownIdentifier) {}
void main() { f(); }

// ==== case: defarg_void_out_good ====
// @expect: accept
void getValue(int &out val = void) { val = 100; }
void main() { getValue(); }

// ==== case: defarg_void_out_normal ====
// @expect: accept
int divide(int a, int b, bool &out success = void) {
    if (b == 0) {
        success = false;
        return 0;
    }
    success = true;
    return a / b;
}
void main() {
    bool ok;
    int q1 = divide(10, 2, ok);
    int q2 = divide(10, 0);
}

// ==== case: defarg_type_mismatch_good ====
// @expect: accept
float scale(float val, double factor = 2.0) { return float(val * factor); }
void main() { float s = scale(1.5f); }

// ==== case: defarg_type_mismatch_normal ====
// @expect: accept
float adjust(float val, float delta = 1) { return val + delta; }
void main() { float a = adjust(2.5f); }

// ==== case: ref_in_good ====
// @expect: accept
int getLen(const string &in s) { return s.length(); }
void main() { int l = getLen("test"); }

// ==== case: ref_in_normal ====
// @expect: accept
int sumElements(const array<int> &in items) {
    int sum = 0;
    for (uint i = 0; i < items.length(); ++i) sum += items[i];
    return sum;
}
void main() {
    array<int> nums = {10, 20, 30};
    int total = sumElements(nums);
}

// ==== case: ref_in_bad ====
// @expect: reject
void mutate(const string &in s) { s = "modified"; }
void main() {}

// ==== case: ref_out_good ====
// @expect: accept
void initVal(int &out x) { x = 42; }
void main() { int a; initVal(a); }

// ==== case: ref_out_normal ====
// @expect: accept
void getMinMax(const array<int> &in arr, int &out minVal, int &out maxVal) {
    if (arr.length() == 0) { minVal = 0; maxVal = 0; return; }
    minVal = arr[0];
    maxVal = arr[0];
    for (uint i = 1; i < arr.length(); ++i) {
        if (arr[i] < minVal) minVal = arr[i];
        if (arr[i] > maxVal) maxVal = arr[i];
    }
}
void main() {
    array<int> data = {5, 2, 9, 1};
    int low, high;
    getMinMax(data, low, high);
}

// ==== case: ref_out_bad ====
// @expect: reject
void setVal(int &out x) { x = 10; }
void main() { setVal(42); }

// ==== case: ref_inout_good ====
// @expect: accept
void appendVal(array<int> &inout arr, int val) { arr.insertLast(val); }
void main() { array<int> a; appendVal(a, 1); }

// ==== case: ref_inout_normal ====
// @expect: accept
void appendAll(array<int> &inout dst, const array<int> &in src) {
    for (uint i = 0; i < src.length(); ++i) dst.insertLast(src[i]);
}
void main() {
    array<int> a = {1, 2};
    array<int> b = {3, 4};
    appendAll(a, b);
}

// ==== case: ref_inout_bad ====
// @expect: reject
void swap(int &inout a, int &inout b) {}
void main() {}

// ==== case: namedarg_basic_good ====
// @expect: accept
int subtract(int a, int b) { return a - b; }
void main() { int r = subtract(b: 5, a: 20); }

// ==== case: namedarg_basic_normal ====
// @expect: accept
int volume(int length, int width, int height) {
    return length * width * height;
}
void main() {
    int v = volume(height: 3, width: 4, length: 5);
}

// ==== case: namedarg_basic_bad ====
// @expect: reject
int subtract(int a, int b) { return a - b; }
void main() { int r = subtract(x: 10, b: 5); }

// ==== case: namedarg_mixed_good ====
// @expect: accept
int calc(int a, int b, int c) { return a + b * c; }
void main() { int res = calc(10, c: 3, b: 2); }

// ==== case: namedarg_mixed_normal ====
// @expect: accept
int configure(int mode, int width = 800, int height = 600, bool fullscreen = false) {
    return mode + width + height + (fullscreen ? 1 : 0);
}
void main() {
    int cfg = configure(1, fullscreen: true);
}

// ==== case: namedarg_mixed_bad ====
// @expect: reject
int calc(int a, int b, int c) { return a + b + c; }
void main() { int res = calc(a: 10, 20, c: 30); }

// ==== case: namedarg_dup_good ====
// @expect: accept
int step(int from, int to) { return to - from; }
void main() { int d = step(from: 1, to: 10); }

// ==== case: namedarg_dup_normal ====
// @expect: accept
class Point {
    int x;
    int y;
    void set(int px, int py) { x = px; y = py; }
}
void main() {
    Point p;
    p.set(py: 20, px: 10);
}

// ==== case: namedarg_dup_bad ====
// @expect: reject
int calc(int a, int b) { return a + b; }
void main() { int res = calc(a: 5, a: 10); }

// ==== case: funcdef_decl_good ====
// @expect: accept
funcdef int UnaryOp(int);
int doubleVal(int x) { return x * 2; }
void main() {
    UnaryOp@ op = @doubleVal;
    int r = op(5);
}

// ==== case: funcdef_decl_normal ====
// @expect: accept
funcdef bool FilterFunc(int);
array<int> filterArray(const array<int> &in src, FilterFunc@ predicate) {
    array<int> result;
    for (uint i = 0; i < src.length(); ++i) {
        if (predicate(src[i])) result.insertLast(src[i]);
    }
    return result;
}
bool isPositive(int n) { return n > 0; }
void main() {
    array<int> data = {-2, 3, -1, 4};
    array<int> pos = filterArray(data, @isPositive);
}

// ==== case: funcdef_decl_bad ====
// @expect: reject
funcdef int UnaryOp(int);
int badFunc(string s) { return s.length(); }
void main() {
    UnaryOp@ op = @badFunc;
}

// ==== case: funcdef_delegate_good ====
// @expect: accept
funcdef void Action();
class Worker {
    void run() {}
}
void main() {
    Worker w;
    Action@ act = Action(w.run);
    act();
}

// ==== case: funcdef_delegate_normal ====
// @expect: accept
funcdef void Incrementer(int);
class Counter {
    int count = 0;
    void add(int delta) { count += delta; }
}
void main() {
    Counter c;
    Incrementer@ inc = Incrementer(c.add);
    inc(5);
    inc(3);
}

// ==== case: funcdef_null_good ====
// @expect: accept
funcdef void Callback();
void main() {
    Callback@ cb = null;
    if (cb is null) {}
}

// ==== case: funcdef_null_normal ====
// @expect: accept
funcdef void Notifier(string);
class Service {
    Notifier@ onNotify;
    void trigger(string msg) {
        if (onNotify !is null) onNotify(msg);
    }
}
void main() {
    Service s;
    s.trigger("test");
}

// ==== case: funcdef_sig_good ====
// @expect: accept
funcdef int BinaryOp(int, int);
int add(int a, int b) { return a + b; }
void main() {
    BinaryOp@ op = @add;
    int x = op(3, 4);
}

// ==== case: funcdef_sig_normal ====
// @expect: accept
funcdef void Mutator(array<int> &inout, int);
void pushVal(array<int> &inout arr, int v) { arr.insertLast(v); }
void main() {
    Mutator@ m = @pushVal;
    array<int> a;
    m(a, 10);
}

// ==== case: funcdef_sig_bad ====
// @expect: reject
funcdef int IntOp();
void voidFunc() {}
void main() {
    IntOp@ op = @voidFunc;
}

// ==== case: funcdef_defarg_good ====
// @expect: accept
funcdef void Cb(int a = 1);
void main() { Cb@ f = null; }

// ==== case: funcdef_defarg_normal ====
// @expect: accept
funcdef void Logger(string prefix, int level = 0);
void logMsg(string prefix, int level) {}
void main() {
    Logger@ l = @logMsg;
    l("test", 1);
}

// ==== case: lambda_basic_good ====
// @expect: accept
funcdef int Transformer(int);
void main() {
    Transformer@ t = function(x) { return x * 2; };
    int v = t(5);
}

// ==== case: lambda_basic_normal ====
// @expect: accept
funcdef int Combiner(int, int);
int fold(const array<int> &in items, int init, Combiner@ op) {
    int acc = init;
    for (uint i = 0; i < items.length(); ++i) acc = op(acc, items[i]);
    return acc;
}
void main() {
    array<int> nums = {1, 2, 3, 4};
    int sum = fold(nums, 0, function(a, b) { return a + b; });
}

// ==== case: lambda_basic_bad ====
// @expect: reject
funcdef int Transformer(int);
void main() {
    Transformer@ t = function(a, b) { return a + b; };
}

// ==== case: lambda_explicit_good ====
// @expect: accept
funcdef int Compute(int, int);
void main() {
    Compute@ c = function(int a, int b) { return a - b; };
    int r = c(10, 4);
}

// ==== case: lambda_explicit_normal ====
// @expect: accept
funcdef void IntHandler(int);
funcdef void StrHandler(string);
void handle(IntHandler@ h) { h(42); }
void handle(StrHandler@ h) { h("hi"); }
void main() {
    handle(function(int v) {});
}

// ==== case: lambda_explicit_bad ====
// @expect: reject
funcdef int Compute(int, int);
void main() {
    Compute@ c = function(string a, int b) { return 0; };
}

// ==== case: lambda_closure_good ====
// @expect: accept
funcdef int Gen();
void main() {
    Gen@ g = function() { int x = 10; return x + 5; };
    int val = g();
}

// ==== case: lambda_closure_normal ====
// @expect: accept
int g_offset = 100;
funcdef int Shift(int);
void main() {
    Shift@ s = function(x) {
        int step = 5;
        return x + step + g_offset;
    };
    int res = s(10);
}

// ==== case: lambda_closure_bad ====
// @expect: reject
funcdef int Fn();
void main() {
    int localVal = 42;
    Fn@ f = function() { return localVal; };
}

// ==== case: lambda_ret_good ====
// @expect: accept
funcdef void Action();
void main() {
    Action@ a = function() {};
    a();
}

// ==== case: lambda_ret_normal ====
// @expect: accept
funcdef array<int>@ MakeArray();
void main() {
    MakeArray@ m = function() {
        array<int> arr = {1, 2, 3};
        return arr;
    };
    array<int>@ res = m();
}

// ==== case: lambda_ret_bad ====
// @expect: reject
funcdef int NumberGen();
void main() {
    NumberGen@ n = function() { return "not a number"; };
}

// ==== case: lambda_auto_good ====
// @expect: accept
funcdef int Op(int);
void main() {
    Op@ op = function(x) { return x * 3; };
    int v = op(4);
}

// ==== case: lambda_auto_normal ====
// @expect: accept
funcdef bool Pred(int);
bool check(int val, Pred@ p) { return p(val); }
void main() {
    bool ok = check(10, function(v) { return v > 5; });
}

// ==== case: lambda_auto_bad ====
// @expect: reject
void main() {
    auto f = function() {};
}

// ==== case: ret_val_good ====
// @expect: accept
int getVal() { return 10; }
void main() { int x = getVal(); }

// ==== case: ret_val_normal ====
// @expect: accept
int findIndex(const array<int> &in arr, int target) {
    for (uint i = 0; i < arr.length(); ++i) {
        if (arr[i] == target) return int(i);
    }
    return -1;
}
void main() {
    array<int> data = {10, 20, 30};
    int idx = findIndex(data, 20);
}

// ==== case: ret_val_bad ====
// @expect: reject
int check(int x) {
    if (x > 0) return 1;
}
void main() {}

// ==== case: ret_void_good ====
// @expect: accept
void doNothing() { return; }
void main() { doNothing(); }

// ==== case: ret_void_normal ====
// @expect: accept
void process(int code) {
    if (code < 0) return;
    int x = code * 2;
}
void main() { process(5); }

// ==== case: ret_void_bad ====
// @expect: reject
void f() { return 42; }
void main() {}

// ==== case: ret_ref_global_good ====
// @expect: accept
int g_counter = 0;
int& getCounter() { return g_counter; }
void main() { getCounter() = 5; }

// ==== case: ret_ref_global_normal ====
// @expect: accept
class Settings { int volume = 50; }
Settings g_settings;
int& getVolume() { return g_settings.volume; }
void main() {
    getVolume() = 80;
    int v = getVolume();
}

// ==== case: ret_ref_global_bad ====
// @expect: reject
int& getLocal() {
    int local = 10;
    return local;
}
void main() {}

// ==== case: ret_ref_member_good ====
// @expect: accept
class Node {
    int value;
    int& getVal() { return value; }
}
void main() {
    Node n;
    n.getVal() = 42;
}

// ==== case: ret_ref_member_normal ====
// @expect: accept
class IntHolder {
    int a;
    int b;
    int& get(int idx) {
        if (idx == 0) return a;
        return b;
    }
}
void main() {
    IntHolder h;
    h.get(0) = 10;
    h.get(1) = 20;
}

// ==== case: ret_ref_member_bad ====
// @expect: reject
int& getParam(int x) {
    return x;
}
void main() {}

// ==== case: const_val_param_good ====
// @expect: accept
int doubleVal(const int x) { return x * 2; }
void main() { int res = doubleVal(5); }

// ==== case: const_val_param_normal ====
// @expect: accept
int area(const int width, const int height) {
    return width * height;
}
void main() {
    int a = area(10, 20);
}

// ==== case: const_val_param_bad ====
// @expect: reject
void mutate(const int x) {
    x = 10;
}
void main() {}

// ==== case: const_ref_param_good ====
// @expect: accept
int len(const string &in s) { return s.length(); }
void main() { int l = len("abc"); }

// ==== case: const_ref_param_normal ====
// @expect: accept
bool contains(const array<int> &in arr, int val) {
    for (uint i = 0; i < arr.length(); ++i) {
        if (arr[i] == val) return true;
    }
    return false;
}
void main() {
    array<int> nums = {1, 2, 3};
    bool has = contains(nums, 2);
}

// ==== case: const_ref_param_bad ====
// @expect: reject
class Holder {
    void mutate() {}
}
void clearHolder(const Holder &in h) {
    h.mutate();
}
void main() {}

// ==== case: const_method_good ====
// @expect: accept
class C {
    int val = 42;
    int get() const { return val; }
}
void main() {
    C c;
    int v = c.get();
}

// ==== case: const_method_normal ====
// @expect: accept
class Account {
    int balance = 100;
    int getBalance() const { return balance; }
    void deposit(int amt) { balance += amt; }
}
void main() {
    Account a;
    a.deposit(50);
    const Account@ ca = a;
    int b = ca.getBalance();
}

// ==== case: const_method_bad ====
// @expect: reject
class Counter {
    int count = 0;
    void reset() const {
        count = 0;
    }
}
void main() {}

// ==== case: const_ret_ref_good ====
// @expect: accept
int g_num = 100;
const int& getNum() { return g_num; }
void main() { int x = getNum(); }

// ==== case: const_ret_ref_normal ====
// @expect: accept
class User {
    string name = "Alice";
    const string& getName() const { return name; }
}
void main() {
    User u;
    string n = u.getName();
}

// ==== case: const_global_good ====
// @expect: accept
int getConstVal() { return 100; }
void main() { int v = getConstVal(); }

// ==== case: const_global_normal ====
// @expect: accept
int getFirst(const array<int> &in arr) {
    if (arr.length() > 0) return arr[0];
    return 0;
}
void main() {
    array<int> a = {7, 8};
    int x = getFirst(a);
}

// ==== case: const_global_bad ====
// @expect: reject
int compute() const { return 42; }
void main() {}

// ==== case: defarg_void_out_bad ====
// @expect: reject
void f(int x = void) {}
void main() { f(); }


// ==== case: defarg_type_mismatch_bad ====
// @expect: reject
void f(int a = "incompatible") {}
void main() { f(); }


// ==== case: funcdef_delegate_bad ====
// @expect: reject
funcdef void Action();
class Worker {
    void run(int x) {}
}
void main() {
    Worker w;
    Action@ act = Action(w.run);
}


// ==== case: funcdef_null_bad ====
// @expect: reject
funcdef void Callback();
class Dummy {}
void main() {
    Dummy d;
    Callback@ cb = @d;
}


// ==== case: funcdef_defarg_bad ====
// @expect: reject
// manual-vs-compiler: compiler does not substitute default arguments when calling via funcdef handle
funcdef void Cb(int a = 1);
void target(int x) {}
void main() {
    Cb@ cb = @target;
    cb();
}


// ==== case: const_ret_ref_bad ====
// @expect: reject
int g_num = 100;
const int& getNum() { return g_num; }
void main() {
    getNum() = 200;
}


