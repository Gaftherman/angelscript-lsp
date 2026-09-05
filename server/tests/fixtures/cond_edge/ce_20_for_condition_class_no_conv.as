// Probes a for-loop condition using a class instance without a boolean conversion operator.
// Objects lacking opConv or opImplConv to bool cannot be used as conditions.
// EXPECT: reject
class Counter {
    int val;
}
void test() {
    Counter c;
    for (int i = 0; c; i++) {
        break;
    }
}
