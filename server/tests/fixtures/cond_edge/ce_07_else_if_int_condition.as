// Probes an else-if branch where the condition expression evaluates to int instead of bool.
// AngelScript does not perform implicit int-to-bool conversion in conditions.
// EXPECT: reject
void test() {
    int x = 2;
    if (x == 1) {
        x = 10;
    } else if (x) {
        x = 20;
    }
}
