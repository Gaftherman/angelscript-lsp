// ==== case: bad_member_as_type ====
// @expect: reject
// @complete- StoneWall
class Tower {
    int StoneWall;
}
void build() {
    St/*$complete*/ x;
}

// ==== case: bad_namespace_as_type ====
// @expect: reject
namespace SpireHaven {
    class BellTower {}
}
void ring(Sp/*$complete*/@ item) {}

// ==== case: bad_unscoped_nested ====
// @expect: reject
// @complete- DriftWood
// @complete- DriftCurrent
namespace River {
    class DriftWood {}
    class DriftCurrent {}
}
void useDrift(Dr/*$complete*/ obj) {}

// ==== case: borderline_base_class ====
// @expect: accept
// @complete+ WindBase
// @complete+ WindRoot
// @complete- WindGust
class WindBase {}
class WindRoot {}
namespace Sky { class WindGust {} }
class Willow : Wi/*$complete*/ {}

// ==== case: borderline_handle_local_var ====
// @expect: accept
// @complete+ MossPatch
// @complete+ MossRock
// @complete- MossFern
class MossPatch {}
class MossRock {}
namespace Bog { class MossFern {} }
void examine() {
    Mo/*$complete*/@ h = null;
}

// ==== case: good_before_func_name ====
// @expect: accept
// @complete+ Brook
// @complete- BrookHidden
class Brook {}
namespace Secret { class BrookHidden {} }
Br/*$complete*/ getBrook() {
    Brook b;
    return b;
}

// ==== case: good_before_function ====
// @expect: accept
// @complete+ Cloud
class Cloud {}
Cl/*$complete*/ function() { return Cloud(); }

// ==== case: good_const_local_handle ====
// @expect: accept
// @complete+ Glade
// @complete+ GladePool
class Glade {}
class GladePool {}
void walk() {
    const Gl/*$complete*/@ h = null;
}

// ==== case: good_const_prefix_param ====
// @expect: accept
// @complete+ Thorn
// @complete+ ThornBush
// @complete- ThornSpike
class Thorn {}
class ThornBush {}
namespace Briar { class ThornSpike {} }
void inspect(const Th/*$complete*/ &in val) {}

// ==== case: good_func_return_handle ====
// @expect: accept
// @complete+ Ember
// @complete+ EmberGlow
// @complete- EmberAsh
class Ember {}
class EmberGlow {}
namespace Vault { class EmberAsh {} }
Em/*$complete*/@ fetchEmber() {
    return null;
}

// ==== case: good_global_var_decl ====
// @expect: accept
// @complete+ Flint
// @complete+ FlintSpark
class Flint {}
class FlintSpark {}
Fl/*$complete*/ g_tool;

// ==== case: good_local_var_decl ====
// @expect: accept
// @complete+ Quill
// @complete+ QuillPen
// @complete- QuillInk
class Quill {}
class QuillPen {}
namespace Desk { class QuillInk {} }
void writeNote() {
    Qu/*$complete*/ note;
}

// ==== case: good_param_type ====
// @expect: accept
// @complete+ Cloud
class Cloud {}
class Cat {}
void use(Cl/*$complete*/ value) {}

// ==== case: good_return_type ====
// @expect: accept
// @complete+ Cloud
// @complete+ Clap
class Cloud {}
class Clap {}
class Zebra {}
Cl/*$complete*/@ makeThing() { return null; }
