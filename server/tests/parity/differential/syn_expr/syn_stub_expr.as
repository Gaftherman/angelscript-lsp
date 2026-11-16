// ==== case: syn_expr_bad_array_index_string_key ====
// @expect: reject
// @predefined: sdk-addons
void main() {
    array<int> a = {1, 2, 3};
    int x = a["0"];
}
