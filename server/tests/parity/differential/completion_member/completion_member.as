// ==== case: bad_different_class_member ====
// @expect: reject
// @complete- EmberGlow
class FireWood {
    int LogWeight;
}
class CoalMine {
    int EmberGlow;
}
void warm(FireWood& f) {
    int x = f.Em/*$complete*/;
}

// ==== case: bad_inherited_private ====
// @expect: reject
class ArmorBase {
    private int ShieldIron;
}
class KnightArmor : ArmorBase {
    void defend() {
        int x = this.Sh/*$complete*/;
    }
}

// ==== case: bad_private_member_outside ====
// @expect: reject
// @complete- SecretCode
// @complete- SecretKey
class VaultBox {
    private int SecretCode;
    private int SecretKey;
    int PublicId;
}
void breach(VaultBox& v) {
    int stolen = v.Se/*$complete*/;
}

// ==== case: bad_private_method_outside ====
// @expect: reject
// @complete- LockTumbler
class LockDoor {
    private void LockTumbler() {}
    void OpenHandle() {}
}
void pick(LockDoor& l) {
    l.Lo/*$complete*/();
}

// ==== case: borderline_inherited_and_own_members ====
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

// ==== case: good_class_member_method ====
// @expect: accept
// @complete+ BrookFlow
// @complete+ BrookRipples
class BrookWater {
    int BrookDepth;
    void BrookFlow() {}
    void BrookRipples() {}
}
void run(BrookWater& b) {
    b.Br/*$complete*/();
}

// ==== case: good_class_member_var ====
// @expect: accept
// @complete+ FlintSpark
// @complete+ FlintEdge
// @complete- FlintMine
class FlintMine {}
class FlintTool {
    int FlintSpark;
    int FlintEdge;
}
void strike(FlintTool& tool) {
    int x = tool.Fl/*$complete*/;
}

// ==== case: good_inherited_member_access ====
// @expect: accept
// @complete+ BloomColor
// @complete+ BloomScent
// @complete- BloomSecret
class FlowerBase {
    int BloomColor;
    int BloomScent;
    private int BloomSecret;
}
class Orchid : FlowerBase {}
void inspect(Orchid& o) {
    int val = o.Bl/*$complete*/;
}

// ==== case: good_method_param_match ====
// @expect: accept
// @complete+ QuillDraw
// @complete+ QuillWrite
class Parchment {
    void QuillDraw(int strokes) {}
    void QuillWrite(int strokes) {}
}
void draw(Parchment& p) {
    p.Qu/*$complete*/(5);
}

// ==== case: good_private_member_inside_method ====
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
