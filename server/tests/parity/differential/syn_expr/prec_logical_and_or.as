// @expect: accept
void main() {
    bool a = true || false && false;
    bool b = (true || false) && false;
}
