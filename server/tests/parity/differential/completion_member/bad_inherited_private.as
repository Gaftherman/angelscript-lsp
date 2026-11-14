// @expect: reject
class ArmorBase {
    private int ShieldIron;
}
class KnightArmor : ArmorBase {
    void defend() {
        int x = this.Sh/*$complete*/;
    }
}
