// Probes declaring two variables of the same type in a for-loop init clause.
// Multiple variable declarations sharing a type specifier are valid in for-init.
// EXPECT: accept
void test() {
    for (int i = 0, j = 1; i < 5; i++) {
        int sum = i + j;
    }
}
