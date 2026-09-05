// Probes a for-loop increment clause consisting of a bare variable with no operator.
// AngelScript permits any valid expression in the increment clause, so this is accepted.
// EXPECT: accept
void test() {
    for (int i = 0; i < 5; i) {
        break;
    }
}
