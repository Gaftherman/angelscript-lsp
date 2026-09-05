// Probes returning values from both inside the try block and inside the catch block.
// Both branches return integers matching the declared function return type.
// EXPECT: accept
int test(int a, int b) {
    try {
        return a / b;
    } catch {
        return -1;
    }
}
