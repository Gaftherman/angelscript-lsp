// Probes nested if statements to ensure the else clause binds to the innermost if statement.
// This is the standard dangling-else resolution and is syntactically valid.
// EXPECT: accept
void test() {
    int x = 1;
    int y = 2;
    if (x > 0)
        if (y > 0)
            y = 3;
        else
            y = 4;
}
