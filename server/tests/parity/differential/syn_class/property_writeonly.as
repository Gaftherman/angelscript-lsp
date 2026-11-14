// @expect: accept
class Box {
    private int val;
    int value {
        set { val = value; }
    }
}
void main() {
    Box b;
    b.value = 3;
}
