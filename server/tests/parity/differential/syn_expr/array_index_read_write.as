// @expect: accept
void main() {
    array<int> a = {10, 20, 30};
    int x = a[0];
    a[1] = x + 5;
}
