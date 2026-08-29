void TakeUint(uint v) {}
void main() {
    int signedVal = 5;
    TakeUint(1);
    TakeUint(signedVal);
    uint u = signedVal;
    print("" + u);
}
