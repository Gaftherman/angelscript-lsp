// Probes initializing a for-loop index variable from a declared global int.
// Initializing a local loop variable from an accessible global int is valid.
// EXPECT: accept
int g_start = 0;
void test() {
    for (int i = g_start; i < 5; i++) {
        int x = i;
    }
}
