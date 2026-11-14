// @expect: reject
class Box {
    int value {
        get const { return 7; }
    }
}
void main() {
    Box b;
    b.value = 5;
}
