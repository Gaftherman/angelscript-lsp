// ==== case: multi_err_two_undeclared_vars ====
// @expect: reject
// @error: 6:9-6:13
// @error: 7:9-7:12
void main() {
	int alpha = 1;
	int beta = 2;
	int a = alpa;
	int b = bta;
}
// @range: strict





// ==== case: multi_err_missing_semis ====
// @expect: reject
// @error: 4:10-4:10
// @error: 5:10-5:10
void main() {
	int x = 1
	int y = 2
	int z = x + y;
}
// @range: strict





// ==== case: multi_err_three_bad_calls ====
// @expect: reject
// @error: 6:1-6:3
// @error: 7:1-7:4
// @error: 8:1-8:4
void foo(int a) {}
void main() {
	fo(1);
	foo();
	foo(1, 2);
}
// @range: strict





// ==== case: multi_err_bad_types_and_assign ====
// @expect: reject
// @error: 4:1-4:5
// @error: 5:1-5:4
void main() {
	flot a = 1.0f;
	itn b = 2;
}
// @range: strict





// ==== case: multi_err_class_members ====
// @expect: reject
// @error: 6:18-6:23
// @error: 7:18-7:24
class Box {
	int width;
	int height;
	int W() { return widht; }
	int H() { return heigth; }
}
// @range: strict





// ==== case: multi_err_two_missing_parens ====
// @expect: reject
// @error: 5:17-5:18
// @error: 6:17-6:18
int add(int a, int b) { return a + b; }
void main() {
	int x = add(1, 2;
	int y = add(3, 4;
}
// @range: strict





// ==== case: multi_err_two_bad_returns ====
// @expect: reject
// @range: loose
int f() { return "bad"; }
float g() { return true; }





// ==== case: long_line_missing_semi ====
// @expect: reject
// @error: 3:126-3:126
void main() {
	int veryLongVariableNameForTesting = 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10 + 11 + 12 + 13 + 14 + 15 + 16 + 17 + 18 + 19 + 20
	int next = 0;
}
// @range: strict





// ==== case: long_line_typo_at_end ====
// @expect: reject
// @error: 4:105-4:120
void main() {
	int accumulatorValue = 100;
	int result = 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10 + 11 + 12 + 13 + 14 + 15 + 16 + 17 + 18 + 19 + 20 + accumulatorValu;
}
// @range: strict





// ==== case: long_line_extra_paren ====
// @expect: reject
// @error: 4:108-4:109
int calc(int x) { return x * 2; }
void main() {
	int outVal = calc(1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10 + 11 + 12 + 13 + 14 + 15 + 16 + 17 + 18 + 19 + 20));
}
// @range: strict





// ==== case: long_line_wrong_op ====
// @expect: reject
// @error: 3:13-3:103
void main() {
	bool flag = (1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10 + 11 + 12 + 13 + 14 + 15 + 16 + 17 + 18 + 19 + 20) = 210;
}
// @range: strict





// ==== case: long_line_valid ====
// @expect: accept
void main() {
	int accumulatorValue = 100;
	int result = 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10 + 11 + 12 + 13 + 14 + 15 + 16 + 17 + 18 + 19 + 20 + accumulatorValue;
}





// ==== case: tab_indent_undeclared_var ====
// @expect: reject
// @error: 4:10-4:14
void main() {
		int count = 5;
		count = cont + 1;
}
// @range: strict





// ==== case: tab_indent_missing_brace ====
// @expect: reject
// @error: 7:17-7:17
void main() {
	if (true) {
		int x = 1;
		// missing brace
}
// @range: strict





// ==== case: tab_indent_typo_keyword ====
// @expect: reject
// @error: 4:3-4:11
void main() {
		for (int i = 0; i < 10; i++) {
			contineu;
		}
}
// @range: strict





// ==== case: tab_indent_double_op ====
// @expect: reject
// @error: 4:15-4:16
void main() {
		int a = 1;
		int b = a ++ 2;
}
// @range: strict





// ==== case: tab_indent_missing_dot ====
// @expect: reject
// @error: 6:9-6:11
// @error: 6:12-6:14
class Item { int id; }
void main() {
	Item it;
	int x = it id;
}
// @range: strict





// ==== case: tab_indent_valid ====
// @expect: accept
void main() {
		int a = 1;
		if (a > 0) {
			a += 2;
		}
}





// ==== case: lambda_undeclared_var ====
// @expect: reject
// @error: 4:35-4:36
funcdef int Op(int);
void main() {
	Op@ fn = function(int x) { return y + 1; };
}
// @range: strict





// ==== case: lambda_missing_semi ====
// @expect: reject
// @error: 4:41-4:42
funcdef int Op(int);
void main() {
	Op@ fn = function(int x) { return x + 1 };
}
// @range: strict





// ==== case: lambda_wrong_return_type ====
// @expect: reject
// @range: loose
funcdef int Op(int);
void main() {
	Op@ fn = function(int x) { return "hi"; };
}





// ==== case: lambda_typo_param_name ====
// @expect: reject
// @error: 4:37-4:40
funcdef int Op(int);
void main() {
	Op@ fn = function(int val) { return vla * 2; };
}
// @range: strict





// ==== case: lambda_missing_paren ====
// @expect: reject
// @range: loose
funcdef int Op(int);
void main() {
	Op@ fn = function(int x { return x; };
}





// ==== case: lambda_valid ====
// @expect: accept
funcdef int Op(int);
void main() {
	Op@ fn = function(int x) { return x + 1; };
	int r = fn(5);
}





// ==== case: class_method_undeclared_member ====
// @expect: reject
// @error: 5:2-5:5
class Player {
	int hp;
	void Damage(int amt) {
		hpp -= amt;
	}
}
// @range: strict





// ==== case: class_method_missing_return ====
// @expect: reject
// @range: loose
class Math {
	int Square(int x) {
		int r = x * x;
	}
}





// ==== case: class_method_typo_this_member ====
// @expect: reject
// @error: 5:7-5:10
class Node {
	int key;
	void Set(int key) {
		this.kye = key;
	}
}
// @range: strict





// ==== case: class_method_wrong_arg_count ====
// @expect: reject
// @error: 4:14-4:17
class Greeter {
	void Say(int a, int b) {}
	void Run() { Say(1); }
}
// @range: strict





// ==== case: class_method_valid ====
// @expect: accept
class Player {
	int hp;
	void Damage(int amt) { hp -= amt; }
}





// ==== case: nested_ns_undeclared_func ====
// @expect: reject
// @error: 8:1-8:12
namespace A {
	namespace B {
		void Helper() {}
	}
}
void main() {
	A::B::Helpr();
}
// @range: strict





// ==== case: nested_ns_missing_colon ====
// @expect: reject
// @error: 9:9-9:10
// @error: 9:11-9:12
namespace A {
	namespace B {
		int val = 42;
	}
}
void main() {
	int x = A:B::val;
}
// @range: strict





// ==== case: nested_ns_typo_inside ====
// @expect: reject
// @error: 4:34-4:40
namespace Outer {
	namespace Inner {
		int Compute(int x) { return x + undecl; }
	}
}
// @range: strict





// ==== case: nested_ns_valid ====
// @expect: accept
namespace Core {
	namespace Math {
		int Zero() { return 0; }
	}
}
void main() {
	int z = Core::Math::Zero();
}





// ==== case: utf8_comment_then_typo ====
// @expect: reject
// @error: 4:9-4:10
void main() {
	// café résumé
	int x = y;
}
// @range: strict





// ==== case: utf8_inline_comment_then_typo ====
// @expect: reject
// @error: 3:30-3:31
void main() {
	int a = 1; /* 日本語 */ int b = c;
}
// @range: strict





// ==== case: utf8_string_then_missing_semi ====
// @expect: reject
// @error: 3:22-3:22
void main() {
	string s = "héllo 🌍"
	int x = 1;
}
// @range: strict





// ==== case: utf8_string_then_undeclared ====
// @expect: reject
// @error: 3:28-3:29
void main() {
	string s = "αβγδ"; int x = z;
}
// @range: strict





// ==== case: utf8_emoji_comment_then_bad_op ====
// @expect: reject
// @error: 4:11-4:12
void main() {
	// 🚀 launch
	int x = 1 +;
}
// @range: strict





// ==== case: utf8_string_and_comment_valid ====
// @expect: accept
void main() {
	// café 🚀
	string s = "héllo 日本語";
	int len = 5;
}





// ==== case: slip_missing_quote ====
// @expect: reject
// @error: 3:12-3:19
void main() {
	string s = "hello;
}
// @range: strict





// ==== case: slip_extra_dot ====
// @expect: reject
// @error: 5:11-5:12
class C { int x; }
void main() {
	C c;
	int v = c..x;
}
// @range: strict





// ==== case: slip_assign_in_cond_typo ====
// @expect: reject
// @error: 4:10-4:11
void main() {
	int x = 1;
	if (x == ) {}
}
// @range: strict





// ==== case: slip_swapped_type_chars ====
// @expect: reject
// @error: 3:1-3:4
void main() {
	bol flag = true;
}
// @range: strict





// ==== case: slip_missing_comma_args ====
// @expect: reject
// @error: 4:15-4:16
int sum(int a, int b) { return a + b; }
void main() {
	int r = sum(1 2);
}
// @range: strict





// ==== case: slip_trailing_comma_call ====
// @expect: reject
// @error: 4:19-4:20
int sum(int a, int b) { return a + b; }
void main() {
	int r = sum(1, 2, );
}
// @range: strict





// ==== case: slip_missing_rbracket ====
// @expect: reject
// @error: 4:14-4:15
void main() {
	array<int> arr = {1, 2, 3};
	int x = arr[0;
}
// @range: strict





// ==== case: slip_extra_semicolon_if ====
// @expect: reject
// @error: 4:10-4:11
void main() {
	int x = 1;
	if (x > 0;) { x = 2; }
}
// @range: strict
