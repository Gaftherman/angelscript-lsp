// Probes a switch statement attempting to switch on a string value.
// AngelScript only supports integral and enum types in switch expressions.
// EXPECT: reject
int test(string str) {
    switch (str) {
        case "a":
            return 1;
        default:
            return 0;
    }
}
