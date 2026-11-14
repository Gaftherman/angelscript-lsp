// @expect: accept
abstract class Shape {
    int area() const { return 0; }
}
class Square : Shape {
    int area() const override { return 4; }
}
void main() {
    Square s;
    Shape@ p = s;
}
