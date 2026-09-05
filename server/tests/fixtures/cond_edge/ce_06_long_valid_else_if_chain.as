// Probes a long, well-formed chain of if, else-if, and else statements.
// Each branch has a valid boolean condition and returning int matches the function type.
// EXPECT: accept
int test(int val) {
    if (val == 0)
        return 0;
    else if (val == 1)
        return 1;
    else if (val == 2)
        return 2;
    else if (val == 3)
        return 3;
    else
        return -1;
}
