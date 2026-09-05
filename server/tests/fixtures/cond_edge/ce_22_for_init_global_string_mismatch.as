// Probes initializing an int loop variable with a global string variable.
// Initializing an int from a string causes a compile-time type mismatch error.
// EXPECT: reject
string g_text = "zero";
void test() {
    for (int i = g_text; i < 5; i++) {
        int x = i;
    }
}
