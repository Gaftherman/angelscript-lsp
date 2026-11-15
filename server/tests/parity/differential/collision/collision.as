// ==== case: block_local_var_shadows_outer_type ====
// @expect: accept
// @complete+ TimberWood
// @complete- CedarPine
class TimberWood {}
namespace Alpine {
    class CedarPine {}
}
void harvest() {
    {
        int TimberWood = 99;
    }
    Ti/*$complete*/ t;
}

// ==== case: good_local_var_shadows_type_root ====
// @expect: accept
// @complete+ EmberFlame
class EmberFlame {}
void ignite() {
    int EmberFlame = 42;
    ::Em/*$complete*/ x;
}

// ==== case: good_same_class_two_namespaces ====
// @expect: accept
// @complete+ Clamp
namespace One { class Clamp {} }
namespace Two { class Clamp {} }
void f() { One::Cl/*$complete*/ x; }

// ==== case: good_same_class_two_ns_inside_ns ====
// @expect: accept
// @complete+ CliffPeak
namespace North {
    class CliffPeak {}
}
namespace South {
    class CliffPeak {}
    void climb() {
        Cl/*$complete*/ c;
    }
}

// ==== case: good_same_class_two_ns_qualified ====
// @expect: accept
// @complete+ BrookBridge
namespace East {
    class BrookBridge {}
}
namespace West {
    class BrookBridge {}
}
void cross() {
    East::Br/*$complete*/ b;
}

// ==== case: ident_prefix_func_and_class_similar ====
// @expect: accept
// @complete+ Breeze
class Breeze {}
void BreezeGust() {}
void blow() {
    Br/*$complete*/ b;
}

// ==== case: ident_prefix_func_call_vs_type ====
// @expect: accept
// @complete+ ChimeType
class ChimeType {}
void ChimeBell() {}
void ring() {
    Ch/*$complete*/@ h = null;
}

// ==== case: member_class_shadowed_by_field ====
// @expect: accept
// @complete+ CanyonRock
// @complete- BoulderStone
namespace Ridge {
    class BoulderStone {}
}
class CanyonRock {}
class Gorge {
    int CanyonRock;
    void explore() {
        ::Ca/*$complete*/ c;
    }
}

// ==== case: ns_two_namespaces_same_class_qualified ====
// @expect: accept
// @complete+ Quill
// @complete- ScrollMark
namespace Feather { class Quill {} }
namespace Scroll { class Quill {} class ScrollMark {} }
void writeDoc() {
    Feather::Qu/*$complete*/ q;
}

// ==== case: scope_class_ns_same_name_local ====
// @expect: accept
// @complete+ River
namespace Valley { class Pebble {} }
namespace River {
    class River {}
    void drift() {
        Ri/*$complete*/ r;
    }
}

// ==== case: scope_class_outer_ns_conflict ====
// @expect: accept
// @complete+ Haven
namespace Haven {
    class Cedar {}
}
namespace Forest {
    class Timber {}
}
namespace Sanctuary {
    class Haven {}
    void rest() {
        Ha/*$complete*/ h;
    }
}

// ==== case: shadow_local_var_shadows_type ====
// @expect: accept
// @complete+ FlintStone
// @complete- SparkCount
class FlintStone {}
void strike() {
    int FlintStone = 10;
    int SparkCount = 5;
    ::Fl/*$complete*/ f;
}

// ==== case: shadow_param_shadows_class_type ====
// @expect: accept
// @complete+ MossLichen
// @complete- SporeCell
namespace Meadow {
    class SporeCell {}
}
namespace Swamp {
    class MossLichen {}
    void cultivate(int MossLichen) {
        Swamp::Mo/*$complete*/ m;
    }
}
