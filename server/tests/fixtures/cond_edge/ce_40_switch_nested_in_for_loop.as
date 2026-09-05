// Probes a switch statement nested inside a for-loop body.
// Placing switch statements inside loop blocks is valid AngelScript control flow.
// EXPECT: accept
void test() {
    for (int i = 0; i < 3; i++) {
        switch (i) {
            case 0:
                break;
            case 1:
                break;
            default:
                break;
        }
    }
}
