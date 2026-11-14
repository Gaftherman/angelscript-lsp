// @expect: accept
int f(array<int>@ arr) {
    int s = 0;
    foreach (int v : arr) {
        s += v;
    }
    return s;
}
