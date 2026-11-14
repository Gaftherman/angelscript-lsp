// @expect: reject
void f(int x) {
    switch (x) {
        case 1:
            continue;
        default:
            break;
    }
}
