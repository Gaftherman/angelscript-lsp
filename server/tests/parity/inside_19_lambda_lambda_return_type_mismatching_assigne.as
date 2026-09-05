funcdef int IntProducer();
void test() {
    IntProducer@ fn = function() {
        return;
    };
}
