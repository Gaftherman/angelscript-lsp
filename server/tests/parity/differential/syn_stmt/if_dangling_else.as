// @expect: accept
int f(bool a, bool b) {
    int r = 0;
    if (a)
        if (b)
            r = 1;
        else
            r = 2;
    return r;
}
