// ==== case: ancestor_access_inner ====
// @expect: accept
namespace Peak {
    class BoulderStone {}
    namespace Ridge {
        void climb() {
            BoulderStone b;
        }
    }
}

// ==== case: ancestor_accessing_child_unqualified ====
// @expect: reject
namespace IslandCoast {
    namespace HiddenCove {
        class PirateClipper {}
    }
    void spyFromCliff() {
        PirateClipper ship;
    }
}

// ==== case: bad_sibling_access ====
// @expect: reject
namespace A { void top() {} }
namespace B { void inner() { top(); } }

// ==== case: compound_namespace_decl ====
// @expect: accept
namespace Meadow::Brook {
    class WaterLily {}
    void driftFlow() {
        WaterLily w;
    }
}

// ==== case: compound_sibling_qualified ====
// @expect: accept
namespace Realm::Forest {
    class OakTree {}
}
namespace Realm::Mountain {
    void gazeTrees() {
        Realm::Forest::OakTree tree;
    }
}

// ==== case: deep_nested_qualified_ancestor ====
// @expect: accept
class GlobalBeacon {}
namespace Cosmos {
    class StarCluster {}
    namespace Galaxy {
        namespace Solar {
            void observeStars() {
                ::GlobalBeacon b;
                Cosmos::StarCluster s;
            }
        }
    }
}

// ==== case: good_ancestor_access ====
// @expect: accept
namespace A { void top() {} namespace B { void inner() { top(); } } }

// ==== case: nested_shadowing_inner ====
// @expect: accept
namespace Forest {
    class LeafFoliage { int outerVal; }
    namespace Glade {
        class LeafFoliage { int innerVal; }
        void checkLeaves() {
            LeafFoliage l;
            l.innerVal = 1;
        }
    }
}

// ==== case: private_member_inside_method ====
// @expect: accept
namespace Citadel {
    class GateLock {
        private int KeyPin;
        void unlockGate() {
            int p = this.KeyPin;
        }
    }
}

// ==== case: private_member_outside_access ====
// @expect: reject
namespace VaultDungeon {
    class SecretChest {
        private int GoldCoins;
    }
    void stealTreasure() {
        SecretChest chest;
        int x = chest.GoldCoins;
    }
}

// ==== case: private_method_other_class ====
// @expect: reject
namespace GardenCourtyard {
    class RoseBush {
        private void bloomFlower() {}
    }
    class Caretaker {
        void tendGarden() {
            RoseBush bush;
            bush.bloomFlower();
        }
    }
}

// ==== case: sibling_access_qualified ====
// @expect: accept
namespace Valley {
    void streamWater() {}
}
namespace Hill {
    void lookoutPoint() {
        Valley::streamWater();
    }
}

// ==== case: sibling_access_unqualified ====
// @expect: reject
namespace NorthTerritory {
    void marchTroops() {}
}
namespace SouthTerritory {
    void patrolBorder() {
        marchTroops();
    }
}

// ==== case: undeclared_function_call ====
// @expect: reject
namespace CavernDeep {
    namespace UnderAbyss {
        void echoSound() {
            unheardWhisperSpell();
        }
    }
}

// ==== case: wrong_qualification_func_call ====
// @expect: reject
namespace SkyDome {
    void soarHigh() {}
    namespace CloudCover {
        void makeRain() {
            SkyDome::thunderStrike();
        }
    }
}

// ==== case: wrong_qualification_compound ====
// Known gap: the server accepts `S::C` for a type declared in `namespace H::S`; the compiler rejects it.
// @expect: reject
namespace HavenRealm::SanctuaryGrounds {
    class SacredShrine {}
}
void visitShrine() {
    SanctuaryGrounds::SacredShrine s;
}
