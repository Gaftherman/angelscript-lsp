// @expect: reject
class Box {
    private int val;
    int value {
        set { val = value; }
    }
}
void main() {
    Box b;
    int x = b.value;
}
