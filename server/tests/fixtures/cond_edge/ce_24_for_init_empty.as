// Probes a for-loop with an empty init clause when the counter is declared before the loop.
// An empty init clause is valid AngelScript syntax.
// EXPECT: accept
void test() {
    int i = 0;
    for (; i < 3; i++) {
        int x = i;
    }
}
