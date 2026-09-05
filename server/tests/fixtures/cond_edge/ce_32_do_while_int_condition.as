// Probes a do-while loop whose condition is an integer expression.
// AngelScript requires loop conditions to be bool and rejects int conditions.
// EXPECT: reject
void test() {
    int count = 3;
    do {
        count--;
    } while (count);
}
