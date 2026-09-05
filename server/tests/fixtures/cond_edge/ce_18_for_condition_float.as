// Probes a for-loop whose condition expression evaluates to float instead of bool.
// Floating point types cannot serve as conditional expressions in AngelScript.
// EXPECT: reject
void test() {
    float f = 1.0f;
    for (int i = 0; f; i++) {
        f -= 0.1f;
    }
}
