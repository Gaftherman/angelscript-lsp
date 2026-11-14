// @expect: accept
class Box {
    int value {
        get const { return 7; }
    }
}
void main() {
    Box b;
    int x = b.value;
}
