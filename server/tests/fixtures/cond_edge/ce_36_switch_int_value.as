// Probes a standard switch statement switching on an integer expression.
// Switching on integer values with valid case labels is accepted by AngelScript.
// EXPECT: accept
int test(int x) {
    switch (x) {
        case 0:
            return 10;
        case 1:
            return 20;
        default:
            return 30;
    }
}
