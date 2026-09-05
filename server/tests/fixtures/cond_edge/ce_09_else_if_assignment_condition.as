// Probes an else-if branch condition using an assignment rather than a comparison.
// Assigning an int does not yield a boolean condition expression.
// EXPECT: reject
void test() {
    int x = 0;
    if (x == 0) {
        x = 2;
    } else if (x = 1) {
        x = 3;
    }
}
