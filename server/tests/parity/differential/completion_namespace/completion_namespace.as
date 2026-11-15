// ==== case: bad_ancestor_skip_level ====
// @expect: reject
// @complete- StonePillar
namespace Alpha {
    namespace Beta {
        namespace Gamma {
            class StonePillar {}
        }
    }
}
void build() {
    Alpha::Gamma::St/*$complete*/ s;
}

// ==== case: bad_sibling_unqualified ====
// @expect: reject
// @complete- BrookTrout
namespace Valley {
    namespace East {
        class BrookTrout {}
    }
    namespace West {
        void fish() {
            Br/*$complete*/ b;
        }
    }
}

// ==== case: bad_three_level_unqualified ====
// @expect: reject
// @complete- EmberCore
namespace Core {
    namespace Engine {
        namespace Sub {
            class EmberCore {}
        }
    }
}
void run() {
    Em/*$complete*/ x;
}

// ==== case: bad_unqualified_nested ====
// @expect: reject
// @complete- Clap
namespace A { namespace B { class Clap {} } }
void f() { Cl/*$complete*/ x; }

// ==== case: bad_using_namespace_syntax ====
// @expect: reject
namespace Glen {
    class FernSpore {}
}
using namespace Glen;
void inspect() {
    Fe/*$complete*/ x;
}

// ==== case: bad_wrong_qualification ====
// @expect: reject
// @complete- QuillHedge
namespace Forest {
    namespace Deep {
        class QuillHedge {}
    }
}
void gather() {
    Deep::Qu/*$complete*/ q;
}

// ==== case: borderline_namespace_same_name_shadowing ====
// @expect: accept
// @complete+ Heath
namespace Moor {
    class Heath {
        int outerVal;
    }
    namespace Deep {
        class Heath {
            int innerVal;
        }
        void check() {
            He/*$complete*/ h;
            int v = h.innerVal;
        }
    }
}

// ==== case: good_enum_qualified_type ====
// @expect: accept
// @complete+ CloudKind
namespace Sky {
    enum CloudKind {
        Stratus,
        Cumulus
    }
}
class CloudStorm {}
void observe(Sky::Cl/*$complete*/ kind) {}

// ==== case: good_enum_value_qualified ====
// @expect: accept
// @complete+ CirrusCloud
// @complete+ CumulusCloud
namespace Sky {
    enum CloudKind {
        CirrusCloud,
        CumulusCloud
    }
}
void test() {
    int k = Sky::C/*$complete*/;
}

// ==== case: good_namespace_shadowing_inner ====
// @expect: accept
// @complete+ PalmLeaf
// @complete+ PalmTree
namespace Oasis {
    class PalmTree {}
    namespace Inner {
        class PalmLeaf {}
        void harvest() {
            Pa/*$complete*/ x;
        }
    }
}

// ==== case: good_qualified ====
// @expect: accept
// @complete+ Clap
namespace A { namespace B { class Clap {} class Zed {} } }
void f() { A::B::Cl/*$complete*/ x; }

// ==== case: good_qualified_in_func_body ====
// @expect: accept
// @complete+ CrystalGlow
// @complete+ CrystalShard
namespace Cavern {
    class CrystalGlow {}
    class CrystalShard {}
}
class CrystalCavern {}
void mine() {
    Cavern::Cr/*$complete*/ c;
}

// ==== case: good_qualified_three_level ====
// @expect: accept
// @complete+ FrostPeak
// @complete+ FrostWind
namespace Realm {
    namespace North {
        namespace Peak {
            class FrostPeak {}
            class FrostWind {}
        }
        class FrostValley {}
    }
}
void trek() {
    Realm::North::Peak::Fr/*$complete*/ x;
}

// ==== case: good_qualified_two_level ====
// @expect: accept
// @complete+ CliffRock
// @complete+ CliffEdge
namespace Ridge {
    class CliffRock {}
    class CliffEdge {}
}
class CliffFallen {}
void climb() {
    Ridge::Cl/*$complete*/ x;
}

// ==== case: good_root_qualified_type ====
// @expect: accept
// @complete+ LarkSong
// @complete+ LarkFeather
class LarkSong {}
class LarkFeather {}
namespace Meadow {
    void sing() {
        ::La/*$complete*/ s;
    }
}

// ==== case: good_sibling_qualified_access ====
// @expect: accept
// @complete+ BrookTrout
// @complete+ BrookSalmon
namespace Valley {
    namespace East {
        class BrookTrout {}
        class BrookSalmon {}
    }
    namespace West {
        void fish() {
            East::Br/*$complete*/ b;
        }
    }
}
