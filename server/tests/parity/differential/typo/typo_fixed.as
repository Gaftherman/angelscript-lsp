// ==== case: local_var_used_before_decl ====
// @expect: reject
// @error: 3:12-3:17
void main() {
    int x = value + 1;
    int value = 5;
}
// @range: strict

// ==== case: namespace_member_typo ====
// @expect: reject
// @error: 6:13-6:28
namespace Input {
    bool isPressed = false;
}
void main() {
    bool p = Input::isPresed;
}
// @range: strict

// ==== case: enum_value_typo ====
// @expect: reject
// @error: 4:14-4:27
enum State { Idle, Running }
void main() {
    State s = State::Runing;
}
// @range: strict

// ==== case: class_method_const_mutation ====
// @expect: reject
// @error: 5:2-5:5
class Counter {
	int val;
	void Inc() const {
		val++;
	}
}
// @range: strict

// ==== case: nested_ns_wrong_ns_name ====
// @expect: reject
// @error: 8:9-8:24
namespace Core {
	namespace Math {
		int Zero() { return 0; }
	}
}
void main() {
	int z = Core::Mth::Zero();
}
// @range: strict

// ==== case: typo_missing_rparen_if ====
// @expect: reject
void f(int a) {
    if (a > 0 {
        a = 1;
    }
}
// @range: strict

// ==== case: typo_missing_rparen_while ====
// @expect: reject
void f(int n) {
    while (n > 0 {
        n--;
    }
}
// @range: strict

// ==== case: typo_missing_colon_ternary ====
// @expect: reject
int f(bool c) {
    return c ? 1 2;
}
// @range: strict
