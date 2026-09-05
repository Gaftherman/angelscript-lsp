funcdef int GetConstant();
void test() {
    GetConstant@ fn = function() {
        return 42;
    };
}
