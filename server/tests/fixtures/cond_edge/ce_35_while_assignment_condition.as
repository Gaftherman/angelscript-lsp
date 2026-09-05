// Probes an assignment expression used as the condition of a while loop.
// Assigning an integer does not produce a boolean condition expression.
// EXPECT: reject
void test() {
    int x = 0;
    while (x = 1) {
        break;
    }
}
