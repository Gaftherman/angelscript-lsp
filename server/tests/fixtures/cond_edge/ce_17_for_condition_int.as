// Probes a for-loop whose condition expression evaluates to int instead of bool.
// AngelScript requires loop conditions to be bool and rejects integer conditions.
// EXPECT: reject
void test() {
    int count = 5;
    for (int i = 0; count; i++) {
        count--;
    }
}
