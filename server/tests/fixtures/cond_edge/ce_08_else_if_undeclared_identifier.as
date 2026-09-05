// Probes an else-if branch whose condition references an undeclared identifier.
// All variables referenced in conditions must be declared beforehand.
// EXPECT: reject
void test() {
    int x = 1;
    if (x == 0) {
        x = 2;
    } else if (undeclaredVar == 5) {
        x = 3;
    }
}
