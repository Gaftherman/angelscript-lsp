// Probes declaring a float variable in the for-loop initialization clause.
// Using a float as the loop control variable is valid in AngelScript.
// EXPECT: accept
void test() {
    for (float f = 0.0f; f < 3.0f; f += 0.5f) {
        float x = f;
    }
}
