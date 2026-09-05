// Probes a for-loop whose condition expression evaluates to a boolean type.
// Boolean loop conditions are the standard expected type and are accepted.
// EXPECT: accept
void test() {
    for (int i = 0; i < 10; i++) {
        int x = i;
    }
}
