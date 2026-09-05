funcdef int SumUp(int);
void test() {
    SumUp@ fn = function(int n) {
        int total = 0;
        for (int i = 0; i < n; i++) {
            total += i;
        }
        return total;
    };
}
