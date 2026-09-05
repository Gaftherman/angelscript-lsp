funcdef int Compute(int);
void test() {
    Compute@ fn = function(int x) {
        int res = x * 2;
        return res;
    };
}
