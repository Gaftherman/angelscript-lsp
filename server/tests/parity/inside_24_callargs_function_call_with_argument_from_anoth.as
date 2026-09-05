int compute() { return 5; }
void target(int x) {}
void test() {
    target(compute());
}
