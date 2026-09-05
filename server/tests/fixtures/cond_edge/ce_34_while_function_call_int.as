// Probes a while loop condition calling a function that returns int instead of bool.
// Loop conditions must evaluate to bool, so returning an int causes a compile-time error.
// EXPECT: reject
int getStatus() {
    return 1;
}
void test() {
    while (getStatus()) {
        break;
    }
}
