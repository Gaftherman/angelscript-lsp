// Probes a for-loop whose condition expression evaluates to a string instead of bool.
// Strings cannot be implicitly converted to boolean loop conditions.
// EXPECT: reject
void test() {
    string s = "looping";
    for (int i = 0; s; i++) {
        break;
    }
}
