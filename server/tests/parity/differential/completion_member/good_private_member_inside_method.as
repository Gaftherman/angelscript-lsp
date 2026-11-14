// @expect: accept
// @complete+ SecretCount
// @complete+ SecretLimit
class Ledger {
    private int SecretCount;
    private int SecretLimit;
    void audit() {
        int s = this.Se/*$complete*/;
    }
}
