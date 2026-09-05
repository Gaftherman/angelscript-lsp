// Probes a basic, well-formed try-catch block for exception handling.
// Standard try-catch syntax is supported and accepted by AngelScript.
// EXPECT: accept
void test() {
    try {
        int a = 10;
        int b = 0;
        int c = a / b;
    } catch {
        int err = -1;
    }
}
