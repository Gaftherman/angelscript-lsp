funcdef int Transform(int);
void test() {
    Transform@ fn = function(int x) {
        switch (x) {
            case 1: return 10;
            case 2: return 20;
            default: return 0;
        }
    };
}
