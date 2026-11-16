// ==== case: vartype_any_store_retrieve_ok ====
// @predefined: sdk-addons
// @expect: accept
void main() {
    any a;
    a.store(5);
    int x;
    a.retrieve(x);
}


// ==== case: vartype_any_ctor_ok ====
// @predefined: sdk-addons
// @expect: accept
void main() {
    any a(5);
    any@ h = any(5);
}


// ==== case: vartype_any_implicit_int_bad ====
// @predefined: sdk-addons
// @expect: reject
void main() {
    any a = 5;
}


// ==== case: vartype_any_assign_any_ok ====
// @predefined: sdk-addons
// @expect: accept
void main() {
    any a;
    any b;
    a = b;
}


// ==== case: vartype_any_handle_null_ok ====
// @predefined: sdk-addons
// @expect: accept
void main() {
    any@ a;
    if (a is null) {}
}


// ==== case: vartype_any_foreach_bad ====
// @predefined: sdk-addons
// @expect: reject
void main() {
    any a;
    foreach (auto v : a) {}
}


// ==== case: vartype_any_arith_bad ====
// @predefined: sdk-addons
// @expect: reject
void main() {
    any a;
    int x = a + 1;
}


// ==== case: vartype_script_param_bad ====
// @expect: reject
void f(?&in v) {}


// ==== case: vartype_script_local_bad ====
// @expect: reject
void main() {
    ? v;
}
