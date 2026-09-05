// Probes a while loop condition calling a function that returns bool.
// Functions returning boolean values are valid conditional expressions.
// EXPECT: accept
bool checkCondition(int val) {
    return val > 0;
}
void test() {
    int x = 3;
    while (checkCondition(x)) {
        x--;
    }
}
