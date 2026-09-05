// Probes a standalone catch block appearing without a preceding try block.
// A catch statement cannot appear in isolation and must follow a try block.
// EXPECT: reject
void test() {
    int x = 1;
    catch {
        x = 2;
    }
}
