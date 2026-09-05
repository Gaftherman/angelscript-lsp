// Probes a while loop using a standard boolean condition expression.
// Boolean conditions in while loops are valid and accepted by AngelScript.
// EXPECT: accept
void test() {
    int x = 0;
    while (x < 5) {
        x++;
    }
}
