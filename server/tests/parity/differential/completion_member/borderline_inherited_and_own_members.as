// @expect: accept
// @complete+ FrostAura
// @complete+ FrostBite
// @complete- FrostHidden
class IceSpell {
    int FrostAura;
    private int FrostHidden;
}
class BlizzardSpell : IceSpell {
    int FrostBite;
}
void castSpell(BlizzardSpell& b) {
    int p = b.Fr/*$complete*/;
}
