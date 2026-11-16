// ==== case: handle_value_assign_bad ====
// @predefined: sdk-addons
// @expect: reject
void main() {
    int a = 1;
    int b = 2;
    @a = @b;
}


// ==== case: str_concat_bad ====
// @predefined: sdk-addons
// @expect: reject
void main() {
    string s = "arr: ";
    array<int> a;
    s = s + a;
}


// ==== case: str_methods_bad ====
// @predefined: sdk-addons
// @expect: reject
void main() {
    string s = "abc";
    s.nonExistentMethod();
}


// ==== case: str_index_bad ====
// @predefined: sdk-addons
// @expect: reject
void main() {
    string s = "hello";
    uint8 c = s["bad"];
}


// ==== case: str_split_bad ====
// @predefined: sdk-addons
// @expect: reject
void main() {
    string s = "a,b";
    s.split(42);
}


// ==== case: arr_template_bad ====
// @predefined: sdk-addons
// @expect: reject
void main() {
    array a;
}


// ==== case: arr_indexing_bad ====
// @predefined: sdk-addons
// @expect: reject
void main() {
    array<int> a = {1, 2};
    int x = a["bad"];
}


// ==== case: arr_methods_bad ====
// @predefined: sdk-addons
// @expect: reject
class Foo {}
void main() {
    array<int> a = {1, 2};
    Foo f;
    a.find(f);
}


// ==== case: conv_prim_explicit_bad ====
// @predefined: sdk-addons
// @expect: reject
void main() {
    string s = "123";
    int x = int(s);
}


// ==== case: str_handle_disallowed_bad ====
// @predefined: sdk-addons
// @expect: reject
// manual-vs-compiler: string is registered as a value type without handle support in SDK
void main() {
    string@ s;
}
