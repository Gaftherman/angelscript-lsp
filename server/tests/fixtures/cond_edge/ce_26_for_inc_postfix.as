// Probes a for-loop increment clause using the postfix increment operator (i++).
// Postfix increment is standard and accepted in loop increment clauses.
// EXPECT: accept
void test() {
    for (int i = 0; i < 5; i++) {
        int x = i;
    }
}
