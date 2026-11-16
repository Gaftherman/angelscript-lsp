// ==== case: handle_value_assign_bad ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: oracle: [(5, 10, 'Object handle is not supported for this type')]
// Reason: needs the host stub for string/array members; the corpus runs without one.
// @expect: reject
void main() {
    int a = 1;
    int b = 2;
    @a = @b;
}


// ==== case: str_concat_bad ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: oracle: [(5, 11, "No matching operator that takes the types 'string' and 'int[]&' found")]
// Reason: needs the host stub for string/array members; the corpus runs without one.
// @expect: reject
void main() {
    string s = "arr: ";
    array<int> a;
    s = s + a;
}


// ==== case: str_methods_bad ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: oracle: [(4, 7, "No matching symbol 'nonExistentMethod'")]
// Reason: needs the host stub for string/array members; the corpus runs without one.
// @expect: reject
void main() {
    string s = "abc";
    s.nonExistentMethod();
}


// ==== case: str_index_bad ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: oracle: [(4, 16, "No matching signatures to 'string::opIndex(const string)'")]
// Reason: needs the host stub for string/array members; the corpus runs without one.
// @expect: reject
void main() {
    string s = "hello";
    uint8 c = s["bad"];
}


// ==== case: str_split_bad ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: oracle: [(4, 7, "No matching signatures to 'string::split(const int)'")]
// Reason: needs the host stub for string/array members; the corpus runs without one.
// @expect: reject
void main() {
    string s = "a,b";
    s.split(42);
}


// ==== case: str_handle_disallowed_bad ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: oracle: [(4, 11, 'Object handle is not supported for this type')]
// Reason: needs the host stub for string/array members; the corpus runs without one.
// @expect: reject
// manual-vs-compiler: string is registered as a value type without handle support in SDK
void main() {
    string@ s;
}


// ==== case: arr_template_bad ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: oracle: [(3, 5, "Template 'array' expects 1 sub type(s)")]
// Reason: needs the host stub for string/array members; the corpus runs without one.
// @expect: reject
void main() {
    array a;
}


// ==== case: arr_indexing_bad ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: oracle: [(4, 14, "No matching signatures to 'array::opIndex(const string)'")]
// Reason: needs the host stub for string/array members; the corpus runs without one.
// @expect: reject
void main() {
    array<int> a = {1, 2};
    int x = a["bad"];
}


// ==== case: arr_methods_bad ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: oracle: [(6, 7, "No matching signatures to 'array::find(Foo&)'")]
// Reason: needs the host stub for string/array members; the corpus runs without one.
// @expect: reject
class Foo {}
void main() {
    array<int> a = {1, 2};
    Foo f;
    a.find(f);
}


// ==== case: conv_prim_explicit_bad ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: oracle: [(4, 13, "Can't implicitly convert from 'string' to 'const int'.")]
// Reason: needs the host stub for string/array members; the corpus runs without one.
// @expect: reject
void main() {
    string s = "123";
    int x = int(s);
}


