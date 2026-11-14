// @expect: reject
// @complete- LockTumbler
class LockDoor {
    private void LockTumbler() {}
    void OpenHandle() {}
}
void pick(LockDoor& l) {
    l.Lo/*$complete*/();
}
