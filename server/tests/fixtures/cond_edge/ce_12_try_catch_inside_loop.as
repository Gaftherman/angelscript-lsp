// Probes placing a valid try-catch block inside a for-loop body.
// Exception handling blocks nested within iteration loops are accepted.
// EXPECT: accept
void test() {
    for (int i = 0; i < 5; i++) {
        try {
            int x = 10 / i;
        } catch {
            break;
        }
    }
}
