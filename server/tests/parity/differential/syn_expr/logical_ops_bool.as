// @expect: accept
void main() {
    bool a = true;
    bool b = false;
    bool c = (a && !b) || (a ^^ b);
}
