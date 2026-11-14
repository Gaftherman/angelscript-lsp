// @expect: accept
enum Color { Red, Green, Blue }
int f(Color c) {
    switch (c) {
        case Red: return 1;
        case Green: return 2;
        default: return 3;
    }
}
