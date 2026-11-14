// @expect: reject
void main() {
    bool cond = true;
    int x = cond ? 1 : "two";
}
