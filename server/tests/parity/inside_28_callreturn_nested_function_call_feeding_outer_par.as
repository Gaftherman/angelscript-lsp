int stepOne(int a) { return a + 1; }
int stepTwo(int b) { return b * 2; }
void test() {
    int result = stepTwo(stepOne(5));
}
