// @expect: reject
class Secret {
    private int code;
}
void main() {
    Secret s;
    s.code = 1;
}
