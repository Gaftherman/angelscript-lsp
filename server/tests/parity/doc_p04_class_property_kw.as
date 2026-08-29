// PROP-01, correctly stated. get_/set_ become the virtual property `V` when declared `property`.
class C
{
    private int m;
    int get_V() const property { return m; }
    void set_V(int v) property { m = v; }
}
void main() { C c; c.V = 3; int a = c.V; print("" + a); }
