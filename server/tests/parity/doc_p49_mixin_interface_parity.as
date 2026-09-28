// Verified by angelscript_oracle: accepted
interface ICombat {
    void Attack(int damage, float range);
    int Power { get; }
}

mixin class MCombatant : ICombat {
    void Attack(int damage, float range) {}
    int get_Power() property { return 100; }
}

class Hero : MCombatant {}

void ExecuteCombat(ICombat@ combatant) {
    combatant.Attack(50, 1.5f);
    int p = combatant.Power;
}

void main() {
    Hero hero;
    hero.Attack(25, 2.0f);
    int hp = hero.Power;
    ExecuteCombat(hero);
}
