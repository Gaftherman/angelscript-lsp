// @expect: accept
class Box {
    private int val;
    int value {
        get const { return val; }
        set { val = value; }
    }
}
void main() {
    Box b;
    b.value = 10;
    int x = b.value;
}
