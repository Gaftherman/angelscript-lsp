// Probes an else if clause missing its conditional expression entirely.
// An else if construct must have an opening and closing parenthesis enclosing a condition.
// EXPECT: reject
void test() {
    int x = 1;
    if (x == 0) {
        x = 2;
    } else if {
        x = 3;
    }
}
