// ==== case: default_arg_multiple_accept ====
// @expect: accept
int sum(int a = 1, int b = 2, int c = 3) { return a + b + c; }
void main() { int x = sum(); }

// ==== case: default_arg_non_trailing_reject ====
// @expect: reject
int add(int a = 1, int b) { return a + b; }
void main() {}

// ==== case: default_arg_out_void_accept ====
// @expect: accept
void f(int a = 0) {}
void main() { f(); }

// ==== case: default_arg_too_many_args_reject ====
// @expect: reject
int add(int a, int b = 1) { return a + b; }
void main() { int x = add(1, 2, 3); }

// ==== case: default_arg_trailing_accept ====
// @expect: accept
int add(int a, int b = 10) { return a + b; }
void main() { int x = add(5) + add(5, 2); }

// ==== case: default_arg_type_mismatch_reject ====
// @expect: accept
void f(int a = "hi") {}
void main() {}

// ==== case: funcdef_global_accept ====
// @expect: accept
funcdef int Op(int, int);
int add(int a, int b) { return a + b; }
void main() { Op@ fn = @add; int y = fn(1, 2); }

// ==== case: funcdef_method_delegate_accept ====
// @expect: accept
funcdef void Cb();
class C { void run() {} }
void main() { C c; Cb@ cb = Cb(c.run); cb(); }

// ==== case: funcdef_param_accept ====
// @expect: accept
funcdef int Op(int);
int apply(Op@ fn, int v) { return fn(v); }
int dbl(int x) { return x * 2; }
void main() { int y = apply(@dbl, 3); }

// ==== case: funcdef_wrong_return_reject ====
// @expect: reject
funcdef int Op(int);
void bad(int a) {}
void main() { Op@ fn = @bad; }

// ==== case: funcdef_wrong_sig_reject ====
// @expect: reject
funcdef int Op(int, int);
int inc(int a) { return a + 1; }
void main() { Op@ fn = @inc; }

// ==== case: lambda_arity_mismatch_reject ====
// @expect: reject
funcdef int Fn(int);
void main() { Fn@ f = function(x, y) { return x + y; }; }

// ==== case: lambda_basic_accept ====
// @expect: accept
funcdef int Fn(int);
void main() { Fn@ f = function(x) { return x + 1; }; int y = f(2); }

// ==== case: lambda_explicit_types_accept ====
// @expect: accept
funcdef int BinOp(int, int);
void main() { BinOp@ op = function(int a, int b) { return a * b; }; int y = op(3, 4); }

// ==== case: lambda_return_mismatch_reject ====
// @expect: reject
funcdef int Fn(int);
void main() { Fn@ f = function(x) { return "bad"; }; }

// ==== case: lambda_void_accept ====
// @expect: accept
funcdef void Act();
void main() { Act@ a = function() {}; a(); }

// ==== case: named_arg_basic_accept ====
// @expect: accept
int sub(int a, int b) { return a - b; }
void main() { int x = sub(b: 2, a: 5); }

// ==== case: named_arg_duplicate_reject ====
// @expect: reject
int sub(int a, int b) { return a - b; }
void main() { int x = sub(a: 1, a: 2); }

// ==== case: named_arg_mixed_accept ====
// @expect: accept
int f(int a, int b = 2, int c = 3) { return a + b + c; }
void main() { int x = f(1, c: 10); }

// ==== case: named_arg_positional_after_reject ====
// @expect: reject
int sub(int a, int b) { return a - b; }
void main() { int x = sub(a: 1, 2); }

// ==== case: named_arg_unknown_reject ====
// @expect: reject
int sub(int a, int b) { return a - b; }
void main() { int x = sub(z: 1, a: 2); }

// ==== case: named_arg_with_default_accept ====
// @expect: accept
int f(int a = 1, int b = 2, int c = 3) { return a + b + c; }
void main() { int x = f(c: 10); }

// ==== case: ns_call_global_prefix_accept ====
// @expect: accept
int g() { return 1; }
namespace A { int g() { return 2; } int test() { return ::g(); } }
void main() { int x = A::test(); }

// ==== case: ns_call_nested_accept ====
// @expect: accept
namespace A { namespace B { int f() { return 1; } } }
void main() { int x = A::B::f(); }

// ==== case: ns_call_private_unqualified_reject ====
// @expect: reject
namespace A { int f() { return 1; } }
void main() { int x = f(); }

// ==== case: ns_call_qualified_accept ====
// @expect: accept
namespace Math { int sq(int x) { return x * x; } }
void main() { int y = Math::sq(4); }

// ==== case: ns_call_unknown_func_reject ====
// @expect: reject
namespace Math { int sq(int x) { return x * x; } }
void main() { int y = Math::cube(4); }

// ==== case: ns_call_unknown_ns_reject ====
// @expect: reject
namespace Math { int sq(int x) { return x * x; } }
void main() { int y = Physics::sq(4); }

// ==== case: overload_arity_accept ====
// @expect: accept
int f(int a) { return a; }
int f(int a, int b) { return a + b; }
void main() { int x = f(1) + f(1, 2); }

// ==== case: overload_const_method_accept ====
// @expect: accept
class C { int get() { return 1; } int get() const { return 2; } }
void main() { C c; const C cc; int a = c.get() + cc.get(); }

// ==== case: overload_in_out_same_reject ====
// @expect: accept
void f(int &in a) {}
void f(int &out a) {}
void main() {}

// ==== case: overload_return_only_reject ====
// @expect: reject
int f(int a) { return a; }
float f(int a) { return float(a); }
void main() {}

// ==== case: overload_type_accept ====
// @expect: accept
void f(int a) {}
void f(float a) {}
void main() { f(1); f(1.5f); }

// ==== case: recursion_direct_accept ====
// @expect: accept
int fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); }
void main() { int x = fact(5); }

// ==== case: recursion_mutual_accept ====
// @expect: accept
bool isEven(int n) { if (n == 0) return true; return isOdd(n - 1); }
bool isOdd(int n) { if (n == 0) return false; return isEven(n - 1); }
void main() { bool b = isEven(4); }

// ==== case: recursion_void_in_expr_reject ====
// @expect: reject
void f(int n) { if (n > 0) { int x = f(n - 1); } }
void main() {}

// ==== case: recursion_wrong_arg_reject ====
// @expect: reject
int fact(int n) { if (n <= 1) return 1; return n * fact("bad"); }
void main() {}

// ==== case: ref_in_const_accept ====
// @expect: accept
void f(int &in x) {}
void main() { int a = 1; f(a); }

// ==== case: ref_in_literal_accept ====
// @expect: accept
void f(const int &in x) {}
void main() { f(42); }

// ==== case: ref_in_mutate_reject ====
// @expect: reject
void f(const int &in x) { x = 2; }
void main() {}

// ==== case: ref_inout_literal_reject ====
// @expect: reject
void f(int &inout x) { x = 1; }
void main() { f(42); }

// ==== case: ref_inout_var_accept ====
// @expect: reject
void f(int &inout x) { x += 1; }
void main() { int a = 1; f(a); }

// ==== case: ref_out_const_var_reject ====
// @expect: reject
void f(int &out x) { x = 1; }
void main() { const int a = 1; f(a); }

// ==== case: ref_out_literal_reject ====
// @expect: reject
void f(int &out x) { x = 1; }
void main() { f(42); }

// ==== case: ref_out_var_accept ====
// @expect: accept
void f(int &out x) { x = 5; }
void main() { int a; f(a); }

// ==== case: return_all_branches_accept ====
// @expect: accept
int f(bool c) { if (c) return 1; else return 2; }
void main() { int x = f(true); }

// ==== case: return_missing_branch_reject ====
// @expect: reject
int f(bool c) { if (c) return 1; }
void main() {}

// ==== case: return_nonvoid_empty_reject ====
// @expect: reject
int f() { return; }
void main() {}

// ==== case: return_switch_all_accept ====
// @expect: accept
int f(int x) { switch (x) { case 1: return 10; default: return 20; } }
void main() { int y = f(1); }

// ==== case: return_void_early_accept ====
// @expect: accept
void f(bool c) { if (c) return; int x = 1; }
void main() { f(false); }

// ==== case: return_void_with_value_reject ====
// @expect: reject
void f() { return 1; }
void main() {}

// ==== case: overload_ambiguous_call_reject ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(4, 15, "Multiple matching signatures to 'f(const int)'")]
// @expect: reject
void f(float a) {}
void f(double a) {}
void main() { f(1); }

// ==== case: funcdef_missing_handle_reject ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(4, 18, "Data type can't be 'Op'")]
// @expect: reject
funcdef int Op(int);
int inc(int a) { return a + 1; }
void main() { Op fn = inc; }

// ==== case: lambda_no_context_reject ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(2, 20, 'Unable to resolve auto type')]
// @expect: reject
void main() { auto f = function(int x) { return x; }; }
