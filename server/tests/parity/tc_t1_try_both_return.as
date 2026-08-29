int f()
{
    try { return 1; }
    catch { return 2; }
}
void main() { print("" + f()); }
