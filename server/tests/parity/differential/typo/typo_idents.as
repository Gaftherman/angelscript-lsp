// ==== case: global_var_dropped_char ====
// @expect: reject
// @error: 4:12-4:20
int g_counter = 0;
void main() {
    int x = g_conter;
}
// @range: strict





// ==== case: global_var_swapped_chars ====
// @expect: reject
// @error: 4:14-4:21
float g_speed = 1.0f;
void main() {
    float s = g_spede;
}
// @range: strict





// ==== case: global_var_wrong_case ====
// @expect: reject
// @error: 4:12-4:22
int g_maxItems = 10;
void main() {
    int n = g_maxitems;
}
// @range: strict





// ==== case: global_var_extra_char ====
// @expect: reject
// @error: 4:8-4:18
bool g_isReady = true;
void main() {
    if (g_isReaddy) {}
}
// @range: strict





// ==== case: local_var_dropped_char ====
// @expect: reject
// @error: 4:12-4:21
void main() {
    int totalScore = 100;
    int x = totalScre + 1;
}
// @range: strict





// ==== case: local_var_swapped_chars ====
// @expect: reject
// @error: 4:12-4:23
void main() {
    int playerIndex = 0;
    int y = playerIdnex;
}
// @range: strict





// ==== case: local_var_wrong_case ====
// @expect: reject
// @error: 4:15-4:24
void main() {
    float deltaTime = 0.016f;
    float dt = deltatime;
}
// @range: strict





// ==== case: local_var_out_of_scope ====
// @expect: reject
// @error: 6:12-6:18
void main() {
    if (true) {
        int scoped = 42;
    }
    int y = scoped;
}
// @range: strict





// ==== case: member_field_dropped_char ====
// @expect: reject
// @error: 7:16-7:21
class Actor {
    float health = 100.0f;
}
void main() {
    Actor a;
    float h = a.heath;
}
// @range: strict





// ==== case: member_field_swapped_chars ====
// @expect: reject
// @error: 7:16-7:22
class Vec2 {
    float length = 0.0f;
}
void main() {
    Vec2 v;
    float l = v.lenght;
}
// @range: strict





// ==== case: member_field_wrong_case ====
// @expect: reject
// @error: 7:16-7:26
class Config {
    int maxRetries = 3;
}
void main() {
    Config cfg;
    int r = cfg.maxretries;
}
// @range: strict





// ==== case: member_field_unknown ====
// @expect: reject
// @error: 7:15-7:21
class Item {
    int id = 1;
}
void main() {
    Item it;
    int w = it.weight;
}
// @range: strict





// ==== case: member_method_dropped_char ====
// @expect: reject
// @error: 7:6-7:11
class Engine {
    void Update() {}
}
void main() {
    Engine e;
    e.Updte();
}
// @range: strict





// ==== case: member_method_wrong_case ====
// @expect: reject
// @error: 7:6-7:11
class Timer {
    void Reset() {}
}
void main() {
    Timer t;
    t.reset();
}
// @range: strict





// ==== case: member_method_unknown ====
// @expect: reject
// @error: 7:8-7:13
class Logger {
    void Info() {}
}
void main() {
    Logger log;
    log.Debug();
}
// @range: strict





// ==== case: member_method_missing_arg ====
// @expect: reject
// @error: 7:14-7:17
class MathUtil {
    int Add(int a, int b) { return a + b; }
}
void main() {
    MathUtil m;
    int s = m.Add(1);
}
// @range: strict





// ==== case: member_method_extra_arg ====
// @expect: reject
// @error: 7:6-7:9
class Counter {
    void Inc(int step) {}
}
void main() {
    Counter c;
    c.Inc(1, 2);
}
// @range: strict





// ==== case: func_call_dropped_char ====
// @expect: reject
// @error: 4:12-4:21
int ComputeSum(int a, int b) { return a + b; }
void main() {
    int s = ComputSum(1, 2);
}
// @range: strict





// ==== case: func_call_swapped_chars ====
// @expect: reject
// @error: 4:4-4:13
void PrintValue(int v) {}
void main() {
    PrintValu(10);
}
// @range: strict





// ==== case: func_call_wrong_case ====
// @expect: reject
// @error: 4:4-4:14
void Initialize() {}
void main() {
    initialize();
}
// @range: strict





// ==== case: func_call_missing_arg ====
// @expect: reject
// @error: 4:14-4:18
float Lerp(float a, float b, float t) { return a + (b - a) * t; }
void main() {
    float r = Lerp(0.0f, 1.0f);
}
// @range: strict





// ==== case: namespace_dropped_char ====
// @expect: reject
// @error: 6:14-6:20
namespace Physics {
    float gravity = 9.81f;
}
void main() {
    float g = Physcs::gravity;
}
// @range: strict





// ==== case: namespace_wrong_case ====
// @expect: reject
// @error: 6:4-6:15
namespace Audio {
    void Mute() {}
}
void main() {
    audio::Mute();
}
// @range: strict





// ==== case: namespace_unknown ====
// @expect: reject
// @error: 6:12-6:20
namespace Render {
    int width = 800;
}
void main() {
    int w = Graphics::width;
}
// @range: strict





// ==== case: type_local_dropped_char ====
// @expect: reject
// @error: 4:4-4:9
class Player {}
void main() {
    Playr p;
}
// @range: strict





// ==== case: type_local_wrong_case ====
// @expect: reject
// @error: 4:4-4:10
class Weapon {}
void main() {
    weapon w;
}
// @range: strict





// ==== case: type_param_typo ====
// @expect: reject
// @error: 3:10-3:19
class Transform {}
void Move(Transfrom@ t) {}
// @range: strict





// ==== case: type_return_typo ====
// @expect: reject
// @error: 3:0-3:5
class Result {}
Reslt@ GetResult() { return null; }
// @range: strict





// ==== case: type_handle_typo ====
// @expect: reject
// @error: 4:4-4:8
class Node {}
void main() {
    Ndoe@ n = null;
}
// @range: strict





// ==== case: type_builtin_typo ====
// @expect: reject
// @error: 3:4-3:9
void main() {
    flaot x = 1.0f;
}
// @range: strict





// ==== case: type_builtin_wrong_case ====
// @expect: reject
// @error: 3:4-3:7
void main() {
    Int x = 1;
}
// @range: strict





// ==== case: enum_name_typo ====
// @expect: reject
// @error: 4:4-4:8
enum Color { Red, Green, Blue }
void main() {
    Colr c = Color::Red;
}
// @range: strict





// ==== case: base_class_typo ====
// @expect: reject
// @error: 3:16-3:25
class BaseEntity {}
class Derived : BaseEntty {}
// @range: strict





// ==== case: interface_typo ====
// @expect: reject
// @error: 5:15-5:23
interface IDrawable {
    void Draw();
}
class Sprite : IDrawble {
    void Draw() {}
}
// @range: strict





// ==== case: param_name_typo_in_body ====
// @expect: reject
// @error: 4:11-4:16
// @error: 4:19-4:24
int Square(int number) {
    return numbr * numbr;
}
// @range: strict





// ==== case: this_member_typo ====
// @expect: reject
// @error: 4:32-4:35
class Box {
    int size = 1;
    int GetSize() { return this.siz; }
}
// @range: strict





// ==== case: accept_forward_func_call ====
// @expect: accept
void main() {
    Helper();
}
void Helper() {}





// ==== case: accept_default_arg_omitted ====
// @expect: accept
int AddOpt(int a, int b = 10) {
    return a + b;
}
void main() {
    int x = AddOpt(5);
}





// ==== case: accept_overloaded_method ====
// @expect: accept
class Printer {
    void Print(int x) {}
    void Print(float x) {}
}
void main() {
    Printer p;
    p.Print(1);
    p.Print(1.5f);
}





// ==== case: accept_case_sensitive_distinct ====
// @expect: accept
void main() {
    int value = 1;
    int Value = 2;
    int sum = value + Value;
}





// ==== case: accept_namespace_qualified ====
// @expect: accept
namespace Math {
    int Abs(int x) { return x < 0 ? -x : x; }
}
void main() {
    int a = Math::Abs(-5);
}





// ==== case: accept_enum_qualified ====
// @expect: accept
enum Mode { Fast, Safe }
void main() {
    Mode m = Mode::Fast;
}
