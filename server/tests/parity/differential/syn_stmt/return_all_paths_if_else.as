// @expect: accept
int f(bool a, bool b) {
    if (a) {
        if (b) return 1;
        else return 2;
    } else {
        return 3;
    }
}
