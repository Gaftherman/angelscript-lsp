// Probes an invalid integer condition inside an if statement placed inside a try block.
// Compile-time type errors inside try blocks are still strictly rejected by the compiler.
// EXPECT: reject
void test() {
    int someInt = 42;
    try {
        if (someInt) {
            int x = 1;
        }
    } catch {
        int y = 2;
    }
}
