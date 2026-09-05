// Probes an else statement without an associated preceding if statement.
// The parser requires an else statement to immediately follow an if statement.
// EXPECT: reject
void test() {
    int x = 1;
    else {
        x = 2;
    }
}
