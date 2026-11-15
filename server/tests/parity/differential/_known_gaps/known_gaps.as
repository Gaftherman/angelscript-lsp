// ==== case: syn_expr_bad_array_index_string_key ====
// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(4, 14, "No matching signatures to 'array::opIndex(const string)'")]
// @expect: reject
void main() {
    array<int> a = {1, 2, 3};
    int x = a["0"];
}
