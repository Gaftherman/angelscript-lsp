// Probes an else-if chain in an int-returning function whose final else returns a string.
// The compiler rejects mismatched return types across control flow branches.
// EXPECT: reject
int test(int x) {
    if (x == 1)
        return 10;
    else if (x == 2)
        return 20;
    else
        return "invalid";
}
