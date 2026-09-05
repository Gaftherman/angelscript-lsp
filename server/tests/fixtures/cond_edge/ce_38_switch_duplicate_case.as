// Probes a switch statement containing duplicate case constant labels.
// Duplicate case labels are detected and rejected by the compiler.
// EXPECT: reject
void test(int x) {
    switch (x) {
        case 1:
            break;
        case 1:
            break;
        default:
            break;
    }
}
