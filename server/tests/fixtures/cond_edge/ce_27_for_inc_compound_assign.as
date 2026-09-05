// Probes a for-loop increment clause using compound assignment (i += 1).
// Compound assignment expressions are accepted in the increment clause.
// EXPECT: accept
void test() {
    for (int i = 0; i < 5; i += 1) {
        int x = i;
    }
}
