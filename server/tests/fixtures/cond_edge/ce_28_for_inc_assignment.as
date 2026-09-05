// Probes a for-loop increment clause using explicit variable reassignment (i = i + 1).
// Assignment expressions are valid within the for-loop increment step.
// EXPECT: accept
void test() {
    for (int i = 0; i < 5; i = i + 1) {
        int x = i;
    }
}
