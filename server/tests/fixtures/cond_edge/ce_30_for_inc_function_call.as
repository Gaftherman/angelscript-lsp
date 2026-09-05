// Probes calling a function declared in the same file within the for-loop increment clause.
// Any expression, including a function call, is permitted in the increment clause.
// EXPECT: accept
int g_counter = 0;
void advance() {
    g_counter++;
}
void test() {
    for (int i = 0; i < 5; advance()) {
        i++;
    }
}
