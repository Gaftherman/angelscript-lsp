// ==== case: typo_missing_dot_member ====
// @expect: reject
// @error: 6:14-6:15
// @error: 6:16-6:17
class Vec { float x; }
void f() {
    Vec v;
    float a = v x;
}
// @range: strict





// ==== case: typo_missing_dot_method ====
// @expect: reject
// @error: 5:4-5:5
class Obj { void ping() {} }
void f() {
    Obj o;
    o ping();
}
// @range: strict





// ==== case: typo_missing_dot_float ====
// @expect: reject
// @error: 3:14-3:15
float g_t = 1.0;
float f = g_t 1;
// @range: strict





// ==== case: typo_missing_semi_var ====
// @expect: reject
// @error: 3:13-3:13
void f() {
    int a = 1
    int b = 2;
}
// @range: strict





// ==== case: typo_missing_semi_return ====
// @expect: reject
// @error: 3:12-3:12
int f() {
    return 1
}
// @range: strict





// ==== case: typo_missing_semi_assign ====
// @expect: reject
// @range: loose
void f() {
    int a = 0;
    a = 5
    a = 6;
}





// ==== case: typo_missing_semi_class_field ====
// @expect: reject
// @range: loose
class C {
    int x
    int y;
}





// ==== case: typo_missing_rparen_call ====
// @expect: reject
// @error: 4:20-4:21
int add(int a, int b) { return a + b; }
void f() {
    int x = add(1, 2;
}
// @range: strict





// ==== case: typo_missing_rparen_decl ====
// @expect: reject
// @error: 2:13-2:14
void f(int a {
}
// @range: strict





// ==== case: typo_missing_rbracket_index ====
// @expect: reject
// @error: 4:17-4:18
void f() {
    int[] arr = {1, 2, 3};
    int x = arr[0;
}
// @range: strict





// ==== case: typo_missing_rbracket_assign ====
// @expect: reject
// @error: 5:8-5:9
// @error: 5:13-5:14
void f() {
    int[] arr = {1, 2};
    arr[1 = 5;
}
// @range: strict





// ==== case: typo_missing_rbrace_func ====
// @expect: reject
// @error: 4:17-4:17
void f() {
    int x = 1;
// @range: strict





// ==== case: typo_missing_rbrace_class ====
// @expect: reject
// @error: 4:17-4:17
class C {
    int a;
// @range: strict





// ==== case: typo_missing_rbrace_if ====
// @expect: reject
// @error: 6:17-6:17
void f(int a) {
    if (a > 0) {
        a = 1;
}
// @range: strict





// ==== case: typo_missing_comma_params ====
// @expect: reject
// @error: 2:13-2:16
void f(int a int b) {
}
// @range: strict





// ==== case: typo_missing_comma_args ====
// @expect: reject
// @error: 4:18-4:19
int add(int a, int b) { return a + b; }
void f() {
    int x = add(1 2);
}
// @range: strict





// ==== case: typo_missing_comma_init ====
// @expect: reject
// @error: 3:19-3:20
void f() {
    int[] arr = {1 2, 3};
}
// @range: strict





// ==== case: typo_missing_comma_vardecl ====
// @expect: reject
// @error: 4:12-4:13
// @error: 4:14-4:15
void f() {
    int a = 1 b = 2;
}
// @range: strict





// ==== case: typo_missing_colon_case ====
// @expect: reject
// @error: 4:14-4:15
void f(int x) {
    switch (x) {
        case 1 break;
    }
}
// @range: strict





// ==== case: typo_missing_colon_default ====
// @expect: reject
// @error: 4:15-4:16
void f(int x) {
    switch (x) {
        default break;
    }
}
// @range: strict





// ==== case: typo_dup_plus_op ====
// @expect: accept
int f(int a, int b) {
    return a + + b;
}





// ==== case: typo_dup_postinc_plus ====
// @expect: accept
int f(int a, int b) {
    return a ++ + b;
}





// ==== case: typo_dup_mul_op ====
// @expect: reject
// @error: 3:15-3:16
int f(int a, int b) {
    return a * * b;
}
// @range: strict





// ==== case: typo_dup_eq_extra ====
// @expect: reject
// @error: 3:16-3:17
bool f(int a, int b) {
    return a == = b;
}
// @range: strict





// ==== case: typo_assign_in_if ====
// @expect: reject
// @error: 3:8-3:13
void f(int a, int b) {
    if (a = b) {
    }
}
// @range: strict





// ==== case: typo_single_amp_in_if ====
// @expect: reject
// @error: 3:13-3:14
bool f(bool a, bool b) {
    return a & b;
}
// @range: strict





// ==== case: typo_dup_arrow_or_dot ====
// @expect: reject
// @error: 5:14-5:15
class C { int x; }
void f() {
    C c;
    int v = c..x;
}
// @range: strict





// ==== case: typo_kw_flaot ====
// @expect: reject
// @error: 2:0-2:5
flaot x = 1.0f;
// @range: strict





// ==== case: typo_kw_retrun ====
// @expect: reject
// @error: 4:4-4:10
// @error: 4:11-4:12
int f() {
    retrun 1;
}
// @range: strict





// ==== case: typo_kw_whlie ====
// @expect: reject
// @error: 4:4-4:9
// @error: 4:18-4:19
void f(int n) {
    whlie (n > 0) {
        n--;
    }
}
// @range: strict





// ==== case: typo_kw_clss ====
// @expect: reject
// @error: 4:0-4:4
// @error: 4:10-4:10
// @error: 6:0-6:1
clss Foo {
    int a;
}
// @range: strict





// ==== case: typo_kw_vodi ====
// @expect: reject
// @error: 3:0-3:4
// @error: 3:5-3:6
vodi f() {
}
// @range: strict





// ==== case: typo_kw_esle ====
// @expect: reject
// @error: 4:14-4:18
// @error: 4:19-4:20
void f(bool c) {
    if (c) {} esle {}
}
// @range: strict





// ==== case: typo_kw_ture ====
// @expect: reject
// @error: 2:9-2:13
bool b = ture;
// @range: strict





// ==== case: typo_kw_fales ====
// @expect: reject
// @error: 2:9-2:14
bool b = fales;
// @range: strict





// ==== case: typo_kw_swtich ====
// @expect: reject
// @error: 5:4-5:10
// @error: 5:15-5:16
// @error: 8:0-8:1
void f(int x) {
    swtich (x) {
        default: break;
    }
}
// @range: strict





// ==== case: typo_kw_brak ====
// @expect: reject
// @error: 4:8-4:12
void f(int x) {
    while (x > 0) {
        brak;
    }
}
// @range: strict





// ==== case: typo_kw_contniue ====
// @expect: reject
// @error: 4:8-4:16
void f(int x) {
    while (x > 0) {
        contniue;
    }
}
// @range: strict





// ==== case: typo_kw_enmu ====
// @expect: reject
// @range: loose
enmu Color {
    Red,
    Green
}





// ==== case: typo_kw_namsespace ====
// @expect: reject
// @error: 4:0-4:10
// @error: 4:15-4:15
// @error: 6:0-6:1
namsespace ns {
    int x = 1;
}
// @range: strict





// ==== case: typo_missing_quote_end ====
// @expect: reject
// @error: 3:15-3:22
void f() {
    string s = "hello;
}
// @range: strict





// ==== case: typo_missing_quote_start ====
// @expect: reject
// @error: 3:20-3:22
void f() {
    string s = hello";
}
// @range: strict





// ==== case: typo_stray_token_num ====
// @expect: reject
// @error: 3:14-3:15
void f() {
    int x = 1 2;
}
// @range: strict





// ==== case: typo_stray_token_ident ====
// @expect: reject
// @error: 3:17-3:18
int f(int a, int b, int c) {
    return a + b c;
}
// @range: strict





// ==== case: typo_stray_token_rparen ====
// @expect: reject
// @error: 3:19-3:20
void f() {
    int x = (1 + 2));
}
// @range: strict





// ==== case: typo_stray_token_rbrace ====
// @expect: reject
// @error: 4:1-4:2
void f() {
    int x = 1;
}}
// @range: strict





// ==== case: typo_accept_double_semi ====
// @expect: accept
void f() {
    int x = 1;;
}





// ==== case: typo_accept_unary_plus ====
// @expect: accept
int f(int a, int b) {
    return a + +b;
}





// ==== case: typo_accept_postinc_plus_space ====
// @expect: accept
int f(int a, int b) {
    return a++ + b;
}





// ==== case: typo_accept_trailing_comma_enum ====
// @expect: accept
enum Color {
    Red,
    Green,
}
