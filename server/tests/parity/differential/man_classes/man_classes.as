// ==== case: class_decl_good ====
// @expect: accept
class Simple {
    int value;
    void setVal(int v) { value = v; }
}
void main() {
    Simple s;
    s.setVal(10);
}

// ==== case: class_decl_normal ====
// @expect: accept
class Player {
    string name;
    int score;
    array<int> history;
    void addScore(int delta) {
        score += delta;
        history.insertLast(delta);
    }
    int getScore() const {
        return score;
    }
}
void main() {
    Player p;
    p.name = "Hero";
    p.addScore(100);
    int s = p.getScore();
}

// ==== case: class_decl_bad ====
// @expect: reject
class DuplicateMember {
    int x;
    string x;
}
void main() {
    DuplicateMember d;
}

// ==== case: class_method_good ====
// @expect: accept
class Box {
    int w;
    void setW(int w) {
        this.w = w;
    }
}
void main() {
    Box b;
    b.setW(5);
}

// ==== case: class_method_normal ====
// @expect: accept
class Account {
    int balance;
    void deposit(int balance) {
        this.balance += balance;
    }
    bool withdraw(int amount) {
        if (amount <= this.balance) {
            this.balance -= amount;
            return true;
        }
        return false;
    }
}
void main() {
    Account acc;
    acc.deposit(100);
    acc.withdraw(50);
}

// ==== case: class_method_bad ====
// @expect: reject
class BadThis {
    int a;
    void test() {
        this.nonExistent = 10;
    }
}
void main() {
    BadThis b;
    b.test();
}

// ==== case: class_const_method_good ====
// @expect: accept
class Value {
    int v;
    int get() const {
        return v;
    }
}
void main() {
    Value v;
    int x = v.get();
}

// ==== case: class_const_method_normal ====
// @expect: accept
class Buffer {
    int size;
    int getSize() { return size; }
    int getSize() const { return size; }
}
void main() {
    Buffer b;
    const Buffer@ h = b;
    int s1 = b.getSize();
    int s2 = h.getSize();
}

// ==== case: class_const_method_bad ====
// @expect: reject
class Mutator {
    int val;
    void badMethod() const {
        val = 42;
    }
}
void main() {
    Mutator m;
}

// ==== case: class_member_init_good ====
// @expect: accept
class Config {
    int timeout = 30;
}
void main() {
    Config c;
}

// ==== case: class_member_init_normal ====
// @expect: accept
class Window {
    int width = 800;
    int height = 600;
    int area = width * height;
    string title = "App";
}
void main() {
    Window w;
}

// ==== case: class_member_init_bad ====
// @expect: reject
class BadInit {
    int x = "not_an_int";
}
void main() {
    BadInit b;
}

// ==== case: class_handle_good ====
// @expect: accept
class Node {}
void main() {
    Node n;
    Node@ h = @n;
}

// ==== case: class_handle_normal ====
// @expect: accept
class Element {
    int id;
    Element@ next;
    Element(int id) { this.id = id; @next = null; }
}
void main() {
    Element first(1);
    Element second(2);
    @first.next = @second;
    if (first.next !is null) {
        int nextId = first.next.id;
    }
}

// ==== case: inherit_single_good ====
// @expect: accept
class Base {
    int x;
}
class Derived : Base {
    int y;
}
void main() {
    Derived d;
    d.x = 1;
    d.y = 2;
}

// ==== case: inherit_single_normal ====
// @expect: accept
class Animal {
    string speak() { return "..."; }
}
class Dog : Animal {
    string speak() { return "woof"; }
}
void main() {
    Dog d;
    Animal@ a = d;
    string sound = a.speak();
}

// ==== case: inherit_single_bad ====
// @expect: reject
class BaseA {}
class BaseB {}
class Derived : BaseA, BaseB {}
void main() {
    Derived d;
}

// ==== case: inherit_super_ctor_good ====
// @expect: accept
class Base {
    Base(int v) {}
}
class Derived : Base {
    Derived() {
        super(10);
    }
}
void main() {
    Derived d;
}

// ==== case: inherit_super_ctor_normal ====
// @expect: accept
class Entity {
    int id;
    string tag;
    Entity(int id, const string &in tag) {
        this.id = id;
        this.tag = tag;
    }
}
class Monster : Entity {
    int hp;
    Monster(int id, const string &in tag, int hp) {
        super(id, tag);
        this.hp = hp;
    }
}
void main() {
    Monster m(1, "Goblin", 50);
}

// ==== case: inherit_base_call_good ====
// @expect: accept
class Base {
    void run() {}
}
class Derived : Base {
    void run() {
        Base::run();
    }
}
void main() {
    Derived d;
    d.run();
}

// ==== case: inherit_base_call_normal ====
// @expect: accept
class TaxCalc {
    int computeTax(int amount) {
        return amount / 10;
    }
}
class SurchargeCalc : TaxCalc {
    int computeTax(int amount) {
        return TaxCalc::computeTax(amount) + 5;
    }
}
void main() {
    SurchargeCalc s;
    int t = s.computeTax(100);
}

// ==== case: inherit_polymorphism_good ====
// @expect: accept
class Base {}
class Derived : Base {}
void main() {
    Derived d;
    Base@ b = d;
    Derived@ d2 = cast<Derived>(b);
}

// ==== case: inherit_polymorphism_normal ====
// @expect: accept
class Shape {
    int area() { return 0; }
}
class Circle : Shape {
    int r;
    Circle(int r) { this.r = r; }
    int area() { return 3 * r * r; }
}
void main() {
    array<Shape@> shapes;
    shapes.insertLast(Circle(5));
    for (uint i = 0; i < shapes.length(); ++i) {
        Circle@ c = cast<Circle>(shapes[i]);
        if (c !is null) {
            int a = c.area();
        }
    }
}

// ==== case: inherit_polymorphism_bad ====
// @expect: reject
class Base {}
class Derived : Base {}
void main() {
    Base b;
    Derived@ d = b;
}

// ==== case: class_final_good ====
// @expect: accept
final class Sealed {
    int val;
}
void main() {
    Sealed s;
    s.val = 1;
}

// ==== case: class_final_normal ====
// @expect: accept
class BaseService {
    void start() {}
}
final class HttpService : BaseService {
    int port;
    HttpService(int p) { port = p; }
    void start() { port = 8080; }
}
void main() {
    HttpService svc(80);
    svc.start();
}

// ==== case: class_final_bad ====
// @expect: reject
final class Sealed {}
class Derived : Sealed {}
void main() {
    Derived d;
}

// ==== case: method_final_good ====
// @expect: accept
class Base {
    void lock() final {}
}
void main() {
    Base b;
    b.lock();
}

// ==== case: method_final_normal ====
// @expect: accept
class Process {
    void run() { step1(); step2(); }
    void step1() final {}
    void step2() {}
}
class CustomProcess : Process {
    void step2() {}
}
void main() {
    CustomProcess cp;
    cp.run();
}

// ==== case: method_final_bad ====
// @expect: reject
class Base {
    void fixed() final {}
}
class Derived : Base {
    void fixed() {}
}
void main() {
    Derived d;
}

// ==== case: class_abstract_good ====
// @expect: accept
abstract class Base {
    void action() {}
}
class Concrete : Base {}
void main() {
    Concrete c;
}

// ==== case: class_abstract_normal ====
// @expect: accept
abstract class Formatter {
    string prefix;
    Formatter(const string &in p) { prefix = p; }
    string format(const string &in msg) {
        return prefix + ": " + msg;
    }
}
class JsonFormatter : Formatter {
    JsonFormatter() { super("JSON"); }
}
void main() {
    JsonFormatter jf;
    string s = jf.format("test");
}

// ==== case: class_abstract_bad ====
// @expect: reject
abstract class AbstractClass {}
void main() {
    AbstractClass a;
}

// ==== case: method_override_good ====
// @expect: accept
class Base {
    void run() {}
}
class Derived : Base {
    void run() override {}
}
void main() {
    Derived d;
    d.run();
}

// ==== case: method_override_normal ====
// @expect: accept
class Widget {
    int getWidth() const { return 100; }
    void draw() {}
}
class Button : Widget {
    int getWidth() const override { return 120; }
    void draw() override {}
}
void main() {
    Button btn;
    Widget@ w = btn;
    int width = w.getWidth();
}

// ==== case: ctor_default_good ====
// @expect: accept
class Simple {
    int x;
    Simple() { x = 42; }
}
void main() {
    Simple s;
}

// ==== case: ctor_default_normal ====
// @expect: accept
class Registry {
    string name;
    array<string> items;
    Registry() {
        name = "default";
        items.insertLast("root");
    }
}
void main() {
    Registry r;
}

// ==== case: ctor_param_good ====
// @expect: accept
class Point {
    int x;
    Point(int x) { this.x = x; }
}
void main() {
    Point p(10);
}

// ==== case: ctor_param_normal ====
// @expect: accept
class Vector3 {
    float x, y, z;
    Vector3(float x, float y, float z) {
        this.x = x; this.y = y; this.z = z;
    }
    Vector3(float scalar) {
        this.x = scalar; this.y = scalar; this.z = scalar;
    }
}
void main() {
    Vector3 v1(1.0f, 2.0f, 3.0f);
    Vector3 v2(0.0f);
}

// ==== case: ctor_param_bad ====
// @expect: reject
class Point {
    Point(int x, int y) {}
}
void main() {
    Point p(10, "bad_arg");
}

// ==== case: ctor_copy_good ====
// @expect: accept
class Value {
    int v;
    Value() { v = 0; }
    Value(const Value &in other) { v = other.v; }
}
void main() {
    Value a;
    Value b(a);
}

// ==== case: ctor_copy_normal ====
// @expect: accept
class Packet {
    array<int> data;
    Packet() {}
    Packet(const Packet &in other) {
        data = other.data;
    }
}
void main() {
    Packet p1;
    p1.data.insertLast(1);
    Packet p2(p1);
}

// ==== case: ctor_copy_bad ====
// @expect: reject
class Item {
    Item(Item other) {}
}
void main() {
    Item a;
}

// ==== case: ctor_explicit_good ====
// @expect: accept
class Meter {
    int m;
    Meter(int m) explicit { this.m = m; }
}
void main() {
    Meter dist(100);
}

// ==== case: ctor_explicit_normal ====
// @expect: accept
class HandleWrapper {
    int id;
    HandleWrapper(int id) explicit { this.id = id; }
}
void inspect(const HandleWrapper &in w) {}
void main() {
    HandleWrapper hw(42);
    inspect(HandleWrapper(10));
}

// ==== case: ctor_explicit_bad ====
// @expect: reject
class Meter {
    int m;
    Meter(int m) explicit { this.m = m; }
}
void check(const Meter &in m) {}
void main() {
    check(100);
}

// ==== case: ctor_delete_good ====
// @expect: accept
class NoDefault {
    NoDefault() delete;
    NoDefault(int x) {}
}
void main() {
    NoDefault n(5);
}

// ==== case: ctor_delete_normal ====
// @expect: accept
class UniqueResource {
    int id;
    UniqueResource(int id) { this.id = id; }
    UniqueResource(const UniqueResource &in) delete;
}
void main() {
    UniqueResource r(1);
}

// ==== case: ctor_delete_bad ====
// @expect: reject
class NoDefault {
    NoDefault() delete;
    NoDefault(int x) {}
}
void main() {
    NoDefault n;
}

// ==== case: dtor_good ====
// @expect: accept
class Resource {
    ~Resource() {}
}
void main() {
    Resource r;
}

// ==== case: dtor_normal ====
// @expect: accept
class FileHolder {
    int handleId;
    FileHolder(int id) { handleId = id; }
    ~FileHolder() {
        handleId = -1;
    }
}
void main() {
    FileHolder fh(12);
}

// ==== case: dtor_bad ====
// @expect: reject
class Resource {
    ~Resource(int a) {}
}
void main() {
    Resource r;
}

// ==== case: prop_accessor_good ====
// @expect: accept
class Widget {
    private int m_x;
    int x {
        get const { return m_x; }
        set { m_x = value; }
    }
}
void main() {
    Widget w;
    w.x = 10;
    int v = w.x;
}

// ==== case: prop_accessor_normal ====
// @expect: accept
class Meter {
    private int m_val;
    int reading {
        get const { return m_val; }
        set {
            if (value < 0) m_val = 0;
            else if (value > 100) m_val = 100;
            else m_val = value;
        }
    }
}
void main() {
    Meter m;
    m.reading = 150;
    int r = m.reading;
}

// ==== case: prop_accessor_bad ====
// @expect: reject
class BadProp {
    int prop {
        get const { return 0; }
        set {}
    }
}
void main() {
    BadProp b;
    b.prop = "not_an_int";
}

// ==== case: prop_get_only_good ====
// @expect: accept
class Circle {
    private float radius;
    Circle(float r) { radius = r; }
    float diameter {
        get const { return radius * 2.0f; }
    }
}
void main() {
    Circle c(5.0f);
    float d = c.diameter;
}

// ==== case: prop_get_only_normal ====
// @expect: accept
class Inventory {
    private array<int> items;
    void add(int item) { items.insertLast(item); }
    uint count {
        get const { return items.length(); }
    }
}
void main() {
    Inventory inv;
    inv.add(10);
    uint n = inv.count;
}

// ==== case: prop_get_only_bad ====
// @expect: reject
class ReadOnly {
    int prop {
        get const { return 42; }
    }
}
void main() {
    ReadOnly r;
    r.prop = 10;
}

// ==== case: prop_set_only_good ====
// @expect: accept
class Sink {
    private int last;
    int input {
        set { last = value; }
    }
}
void main() {
    Sink s;
    s.input = 100;
}

// ==== case: prop_set_only_normal ====
// @expect: accept
class Auth {
    private string secret;
    string password {
        set { secret = value; }
    }
}
void main() {
    Auth a;
    a.password = "secret123";
}

// ==== case: prop_set_only_bad ====
// @expect: reject
class WriteOnly {
    private int v;
    int data {
        set { v = value; }
    }
}
void main() {
    WriteOnly w;
    int x = w.data;
}

// ==== case: prop_func_syntax_good ====
// @expect: accept
class Counter {
    private int m_count;
    int get_count() const property { return m_count; }
    void set_count(int val) property { m_count = val; }
}
void main() {
    Counter c;
    c.count = 5;
    int x = c.count;
}

// ==== case: prop_func_syntax_normal ====
// @expect: accept
class Volume {
    private int m_level;
    int get_level() const property { return m_level; }
    void set_level(int val) property {
        m_level = (val < 0) ? 0 : val;
    }
}
void main() {
    Volume v;
    v.level += 10;
}

// ==== case: prop_func_syntax_bad ====
// @expect: reject
class Mismatched {
    int get_val() const property { return 0; }
    void set_val(string val) property {}
}
void main() {
    Mismatched m;
    m.val = 10;
}

// ==== case: prop_indexed_good ====
// @expect: accept
class StringMap {
    private array<string> data;
    string get_opIndex(int idx) const property { return data[idx]; }
    void set_opIndex(int idx, const string &in val) property { data[idx] = val; }
}
void main() {
    StringMap m;
}

// ==== case: prop_indexed_normal ====
// @expect: accept
class GridRow {
    private array<int> cells;
    GridRow() { cells.resize(10); }
    int get_opIndex(int col) const property {
        return cells[col];
    }
    void set_opIndex(int col, int val) property {
        cells[col] = val;
    }
}
void main() {
    GridRow row;
    row[3] = 42;
    int val = row[3];
}

// ==== case: prop_indexed_bad ====
// @expect: reject
class BadIndex {
    int get_opIndex() const property { return 0; }
}
void main() {
    BadIndex b;
    int x = b[0];
}

// ==== case: op_unary_good ====
// @expect: accept
class Negatable {
    int val;
    Negatable() { val = 0; }
    Negatable(int v) { val = v; }
    Negatable opNeg() const {
        return Negatable(-val);
    }
}
void main() {
    Negatable a(5);
    Negatable b = -a;
}

// ==== case: op_unary_normal ====
// @expect: accept
class StepCounter {
    int count;
    StepCounter() { count = 0; }
    StepCounter& opPreInc() {
        ++count;
        return this;
    }
    StepCounter opPostInc() {
        StepCounter copy = this;
        ++count;
        return copy;
    }
}
void main() {
    StepCounter sc;
    ++sc;
    sc++;
}

// ==== case: op_cmp_good ====
// @expect: accept
class Score {
    int val;
    Score(int v) { val = v; }
    bool opEquals(const Score &in o) const { return val == o.val; }
    int opCmp(const Score &in o) const { return val - o.val; }
}
void main() {
    Score s1(10);
    Score s2(20);
    bool eq = (s1 == s2);
    bool lt = (s1 < s2);
}

// ==== case: op_cmp_normal ====
// @expect: accept
class Score {
    int val;
    Score(int v) { val = v; }
    bool opEquals(int v) const { return val == v; }
    int opCmp(int v) const { return val - v; }
}
void main() {
    Score s(100);
    if (s == 100 && s > 50) {}
}

// ==== case: op_cmp_bad ====
// @expect: reject
class BadEquals {
    void opEquals(const BadEquals &in o) const {}
}
void main() {
    BadEquals a, b;
    bool eq = (a == b);
}

// ==== case: op_assign_good ====
// @expect: accept
class Box {
    int val;
    Box& opAssign(const Box &in o) {
        val = o.val;
        return this;
    }
}
void main() {
    Box a, b;
    b = a;
}

// ==== case: op_assign_normal ====
// @expect: accept
class Accumulator {
    int total;
    Accumulator& opAddAssign(int delta) {
        total += delta;
        return this;
    }
}
void main() {
    Accumulator acc;
    acc += 10;
}

// ==== case: op_assign_bad ====
// @expect: reject
class NonAssignable {
    NonAssignable& opAssign(const NonAssignable &in) delete;
}
void main() {
    NonAssignable a, b;
    b = a;
}

// ==== case: op_binary_good ====
// @expect: accept
class Vec2 {
    int x, y;
    Vec2() { x = 0; y = 0; }
    Vec2(int x, int y) { this.x = x; this.y = y; }
    Vec2 opAdd(const Vec2 &in o) const {
        return Vec2(x + o.x, y + o.y);
    }
}
void main() {
    Vec2 a(1, 2);
    Vec2 b(3, 4);
    Vec2 c = a + b;
}

// ==== case: op_binary_normal ====
// @expect: accept
class Scaled {
    int v;
    Scaled() { v = 0; }
    Scaled(int v) { this.v = v; }
    Scaled opAdd(int scalar) const { return Scaled(v + scalar); }
    Scaled opAdd_r(int scalar) const { return Scaled(v + scalar); }
}
void main() {
    Scaled s(10);
    Scaled r1 = s + 5;
    Scaled r2 = 5 + s;
}

// ==== case: op_binary_bad ====
// @expect: reject
class BadAdd {
    BadAdd opAdd() const { return this; }
}
void main() {
    BadAdd a, b;
    BadAdd c = a + b;
}

// ==== case: op_index_good ====
// @expect: accept
class ListWrapper {
    int opIndex(int idx) const { return idx * 2; }
}
void main() {
    ListWrapper lw;
    int x = lw[3];
}

// ==== case: op_index_normal ====
// @expect: accept
class IntArray5 {
    private array<int> data;
    IntArray5() { data.resize(5); }
    int& opIndex(int idx) { return data[idx]; }
    const int& opIndex(int idx) const { return data[idx]; }
}
void main() {
    IntArray5 arr;
    arr[0] = 10;
    int v = arr[0];
}

// ==== case: op_index_bad ====
// @expect: reject
class NotIndexable {}
void main() {
    NotIndexable obj;
    int x = obj[0];
}

// ==== case: op_call_good ====
// @expect: accept
class Multiplier {
    int factor;
    Multiplier(int f) { factor = f; }
    int opCall(int x) const { return x * factor; }
}
void main() {
    Multiplier m(3);
    int res = m(5);
}

// ==== case: op_call_normal ====
// @expect: accept
class AccumulatingFilter {
    int total;
    bool opCall(int item, int threshold) {
        if (item > threshold) {
            total += item;
            return true;
        }
        return false;
    }
}
void main() {
    AccumulatingFilter filter;
    bool passed = filter(10, 5);
}

// ==== case: op_call_bad ====
// @expect: reject
class NotCallable {}
void main() {
    NotCallable nc;
    nc(1, 2);
}

// ==== case: op_conv_good ====
// @expect: accept
class BoxedInt {
    int v;
    BoxedInt(int v) { this.v = v; }
    int opConv() const { return v; }
}
void main() {
    BoxedInt b(42);
    int x = int(b);
}

// ==== case: op_conv_normal ====
// @expect: accept
class Weight {
    int grams;
    Weight(int g) { grams = g; }
    int opImplConv() const { return grams; }
}
void main() {
    Weight w(500);
    int total = w + 100;
}

// ==== case: op_cast_good ====
// @expect: accept
class Target {}
class Source {
    Target t;
    Target@ opCast() { return @t; }
}
void main() {
    Source s;
    Target@ t = cast<Target>(s);
}

// ==== case: op_cast_normal ====
// @expect: accept
class ServiceHandle {}
class ServiceWrapper {
    ServiceHandle h;
    ServiceHandle@ opImplCast() { return @h; }
}
void consume(ServiceHandle@ sh) {}
void main() {
    ServiceWrapper sw;
    consume(sw);
}

// ==== case: op_for_good ====
// @expect: accept
class Range {
    int max;
    Range(int m) { max = m; }
    int opForBegin() { return 0; }
    bool opForEnd(int it) const { return it >= max; }
    int opForNext(int it) { return it + 1; }
    int opForValue(int it) { return it; }
}
void main() {
    Range r(5);
    foreach (auto v : r) {}
}

// ==== case: op_for_normal ====
// @expect: accept
class KeyValContainer {
    array<string> keys;
    array<int> values;
    uint opForBegin() { return 0; }
    bool opForEnd(uint it) const { return it >= keys.length(); }
    uint opForNext(uint it) { return it + 1; }
    string opForValue0(uint it) { return keys[it]; }
    int opForValue1(uint it) { return values[it]; }
}
void main() {
    KeyValContainer c;
    foreach (auto k, auto v : c) {}
}

// ==== case: interface_decl_good ====
// @expect: accept
interface ITask {
    void execute();
}
class Task : ITask {
    void execute() {}
}
void main() {
    Task t;
    ITask@ it = t;
    it.execute();
}

// ==== case: interface_decl_normal ====
// @expect: accept
interface IRepository {
    int count() const;
    string get(int id);
    void add(int id, const string &in name);
}
class MemoryRepo : IRepository {
    int count() const { return 0; }
    string get(int id) { return ""; }
    void add(int id, const string &in name) {}
}
void main() {
    MemoryRepo mr;
    IRepository@ repo = mr;
    repo.add(1, "test");
}

// ==== case: interface_decl_bad ====
// @expect: reject
interface ITask {
    void execute();
}
class IncompleteTask : ITask {}
void main() {
    IncompleteTask t;
}

// ==== case: interface_multiple_good ====
// @expect: accept
interface IReader { void read(); }
interface IWriter { void write(); }
class Stream : IReader, IWriter {
    void read() {}
    void write() {}
}
void main() {
    Stream s;
    IReader@ r = s;
    IWriter@ w = s;
}

// ==== case: interface_multiple_normal ====
// @expect: accept
interface IIdentifiable { int getId() const; }
interface INamed { string getName() const; }
class User : IIdentifiable, INamed {
    private int m_id;
    private string m_name;
    User(int id, const string &in name) { m_id = id; m_name = name; }
    int getId() const { return m_id; }
    string getName() const { return m_name; }
}
void printName(INamed@ n) { string s = n.getName(); }
void main() {
    User u(1, "Alice");
    printName(u);
}

// ==== case: interface_multiple_bad ====
// @expect: reject
interface IA { void fa(); }
interface IB { void fb(); }
class Partial : IA, IB {
    void fa() {}
}
void main() {
    Partial p;
}

// ==== case: interface_prop_good ====
// @expect: accept
interface IHasValue {
    int value { get const; set; }
}
class Box : IHasValue {
    private int v;
    int value {
        get const { return v; }
        set { v = value; }
    }
}
void main() {
    Box b;
    IHasValue@ h = b;
    h.value = 10;
}

// ==== case: interface_prop_normal ====
// @expect: accept
interface IHasTitle {
    string title { get const; }
}
class Document : IHasTitle {
    private string m_title;
    Document(const string &in t) { m_title = t; }
    string title {
        get const { return m_title; }
    }
}
void main() {
    Document doc("Report");
    IHasTitle@ h = doc;
    string t = h.title;
}

// ==== case: mixin_decl_good ====
// @expect: accept
mixin class M {
    int value;
}
class C : M {}
void main() {
    C c;
    c.value = 42;
}

// ==== case: mixin_decl_normal ====
// @expect: accept
mixin class Timestamped {
    int createdTime;
    void stamp(int now) { createdTime = now; }
}
class Record : Timestamped {
    string data;
    Record(const string &in d, int now) {
        data = d;
        stamp(now);
    }
}
void main() {
    Record r("hello", 100);
}

// ==== case: mixin_decl_bad ====
// @expect: reject
mixin class M {
    void test() {}
}
void main() {
    M m;
}

// ==== case: mixin_override_good ====
// @expect: accept
mixin class M {
    int getVal() { return 1; }
}
class C : M {
    int getVal() { return 2; }
}
void main() {
    C c;
    int v = c.getVal();
}

// ==== case: mixin_override_normal ====
// @expect: accept
mixin class Notifier {
    void notify() {
        onNotify("event");
    }
}
class Listener : Notifier {
    void onNotify(const string &in event) {}
}
void main() {
    Listener l;
    l.notify();
}

// ==== case: mixin_override_bad ====
// @expect: reject
class Base {}
mixin class M : Base {}
class C : M {}
void main() {
    C c;
}

// ==== case: mixin_interface_good ====
// @expect: accept
interface IAction {
    void execute();
}
mixin class ActionMixin : IAction {
    void execute() {}
}
class ConcreteAction : ActionMixin {}
void main() {
    ConcreteAction ca;
    IAction@ a = ca;
    a.execute();
}

// ==== case: mixin_interface_normal ====
// @expect: accept
interface IService {
    void start();
    void stop();
}
mixin class ServiceMixin : IService {
    void start() {}
}
class RealService : ServiceMixin {
    void stop() {}
}
void main() {
    RealService s;
    IService@ isvc = s;
    isvc.start();
    isvc.stop();
}

// ==== case: mixin_interface_bad ====
// @expect: reject
interface IReq {
    void req();
}
mixin class M : IReq {}
class C : M {}
void main() {
    C c;
}

// ==== case: shared_class_good ====
// @expect: accept
shared class SharedData {
    int x;
    void set(int val) { x = val; }
}
void main() {
    SharedData s;
    s.set(10);
}

// ==== case: shared_class_normal ====
// @expect: accept
shared class Message {
    int id;
    string content;
    Message(int id, const string &in content) {
        this.id = id;
        this.content = content;
    }
    string summary() const {
        return content;
    }
}
void main() {
    Message m(1, "hello");
    string s = m.summary();
}

// ==== case: shared_class_bad ====
// @expect: reject
class NonSharedLocal {
    int a;
}
shared class SharedBad {
    NonSharedLocal local;
}
void main() {
    SharedBad s;
}

// ==== case: external_shared_good ====
// @expect: reject
// manual-vs-compiler: manual documents external shared syntax, compiler rejects when not pre-compiled in a prior module
external shared class PrecompiledClass;

// ==== case: external_shared_normal ====
// @expect: reject
// manual-vs-compiler: manual documents external shared class used via handle, compiler rejects when not pre-compiled in a prior module
external shared class PrecompiledService;
void main() {
    PrecompiledService@ svc;
}

// ==== case: external_shared_bad ====
// @expect: reject
external shared class MalformedClass {
    int x;
}

// ==== case: access_private_prop_good ====
// @expect: accept
class Encapsulated {
    private int secret;
    Encapsulated() { secret = 42; }
    int getSecret() const { return secret; }
}
void main() {
    Encapsulated e;
    int s = e.getSecret();
}

// ==== case: access_private_prop_normal ====
// @expect: accept
class Wallet {
    private int coins;
    private string owner;
    Wallet(const string &in owner) {
        this.owner = owner;
        this.coins = 0;
    }
    void addCoins(int c) {
        if (c > 0) coins += c;
    }
    int getCoins() const { return coins; }
}
void main() {
    Wallet w("Bob");
    w.addCoins(50);
}

// ==== case: access_private_prop_bad ====
// @expect: reject
class SecretHolder {
    private int secret;
}
void main() {
    SecretHolder sh;
    sh.secret = 100;
}

// ==== case: access_private_method_good ====
// @expect: accept
class Worker {
    private void internalWork() {}
    void doWork() {
        internalWork();
    }
}
void main() {
    Worker w;
    w.doWork();
}

// ==== case: access_private_method_normal ====
// @expect: accept
class BaseEngine {
    private void prime() {}
    void start() {
        prime();
    }
}
class CarEngine : BaseEngine {
    void run() {
        start();
    }
}
void main() {
    CarEngine ce;
    ce.run();
}

// ==== case: access_private_method_bad ====
// @expect: reject
class BaseEngine {
    private void prime() {}
}
class CarEngine : BaseEngine {
    void run() {
        prime();
    }
}
void main() {
    CarEngine ce;
}

// ==== case: access_protected_good ====
// @expect: accept
class Base {
    protected int count;
}
class Derived : Base {
    void inc() {
        count++;
    }
}
void main() {
    Derived d;
    d.inc();
}

// ==== case: access_protected_normal ====
// @expect: accept
class Layer {
    protected int depth;
    protected void setupDepth(int d) { depth = d; }
}
class UILayer : Layer {
    UILayer(int d) {
        setupDepth(d);
    }
    int getDepth() const { return depth; }
}
void main() {
    UILayer ui(2);
    int d = ui.getDepth();
}

// ==== case: access_protected_bad ====
// @expect: reject
class Guarded {
    protected int value;
}
void main() {
    Guarded g;
    g.value = 5;
}

// ==== case: class_handle_bad ====
// @expect: reject
class TypeA {}
class TypeB {}
void main() {
    TypeA a;
    TypeB@ b = @a;
}


// ==== case: inherit_super_ctor_bad ====
// @expect: reject
class Base {
    Base(int v) {}
}
class Derived : Base {
    Derived() {
        super("invalid_arg_type");
    }
}
void main() {
    Derived d;
}


// ==== case: inherit_base_call_bad ====
// @expect: reject
class Base {}
class Derived : Base {
    void run() {
        Base::noSuch();
    }
}
void main() {
    Derived d;
}


// ==== case: method_override_bad ====
// @expect: reject
class Base {
    void run() {}
}
class Derived : Base {
    void run(int x) override {}
}
void main() {
    Derived d;
}


// ==== case: ctor_default_bad ====
// @expect: reject
class Simple {
    void Simple() {}
}
void main() {
    Simple s;
}


// ==== case: op_unary_bad ====
// @expect: reject
class BadUnary {
    int opNeg(int x) const { return -x; }
}
void main() {
    BadUnary b;
    -b;
}


// ==== case: op_conv_bad ====
// @expect: reject
class BadConv {
    int opConv(int extra) const { return extra; }
}
void main() {
    BadConv b;
    int x = int(b);
}


// ==== case: op_cast_bad ====
// @expect: reject
class BadCast {
    int opCast() { return 0; }
}
void main() {
    BadCast b;
    int x = cast<int>(b);
}


// ==== case: op_for_bad ====
// @expect: reject
class IncompleteContainer {
    int opForBegin() { return 0; }
    int opForNext(int it) { return it + 1; }
    int opForValue(int it) { return it; }
}
void main() {
    IncompleteContainer c;
    foreach (auto v : c) {}
}


// ==== case: interface_prop_bad ====
// @expect: reject
interface IHasNum {
    int num { get const; set; }
}
class BadBox : IHasNum {
    string num {
        get const { return ""; }
        set {}
    }
}
void main() {
    BadBox b;
}




// ==== case: base_prop_spaced_call ====
// @expect: accept
class BaseEntity { void ItemPreFrame() {} void Spawn(int n) {} }
class ScriptBaseItemEntity {
    private BaseEntity@ m_pBaseClass;
    BaseEntity@ BaseClass { get const { return m_pBaseClass; } }
}
class W : ScriptBaseItemEntity {
    void ItemPreFrame() { BaseClass . ItemPreFrame (); }
}


// ==== case: base_prop_dot_space_call ====
// @expect: accept
class BaseEntity { void ItemPreFrame() {} void Spawn(int n) {} }
class ScriptBaseItemEntity {
    private BaseEntity@ m_pBaseClass;
    BaseEntity@ BaseClass { get const { return m_pBaseClass; } }
}
class W : ScriptBaseItemEntity {
    void ItemPreFrame() { BaseClass .ItemPreFrame(); }
}


// ==== case: base_prop_newline_call ====
// @expect: accept
class BaseEntity { void ItemPreFrame() {} void Spawn(int n) {} }
class ScriptBaseItemEntity {
    private BaseEntity@ m_pBaseClass;
    BaseEntity@ BaseClass { get const { return m_pBaseClass; } }
}
class W : ScriptBaseItemEntity {
    void ItemPreFrame() { BaseClass
        .ItemPreFrame(); }
}


// ==== case: base_prop_missing_method_bad ====
// @expect: reject
class BaseEntity { void ItemPreFrame() {} void Spawn(int n) {} }
class ScriptBaseItemEntity {
    private BaseEntity@ m_pBaseClass;
    BaseEntity@ BaseClass { get const { return m_pBaseClass; } }
}
class W : ScriptBaseItemEntity {
    void ItemPreFrame() { BaseClass . NoExiste (); }
}


// ==== case: base_prop_arity_bad ====
// @expect: reject
class BaseEntity { void ItemPreFrame() {} void Spawn(int n) {} }
class ScriptBaseItemEntity {
    private BaseEntity@ m_pBaseClass;
    BaseEntity@ BaseClass { get const { return m_pBaseClass; } }
}
class W : ScriptBaseItemEntity {
    void ItemPreFrame() { BaseClass . Spawn (); }
}


// ==== case: base_prop_outside_class_bad ====
// @expect: reject
class BaseEntity { void ItemPreFrame() {} void Spawn(int n) {} }
class ScriptBaseItemEntity {
    private BaseEntity@ m_pBaseClass;
    BaseEntity@ BaseClass { get const { return m_pBaseClass; } }
}
void main() {
    BaseClass . ItemPreFrame ();
}
