class Box { void put(int v) {} void put(string v) {} }
class Thing {}
void main() { Box b; Thing t; b.put(t); }
