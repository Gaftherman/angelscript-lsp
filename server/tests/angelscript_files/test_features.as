// 1. Interfaces
interface IEntity {
    void Spawn(Vector3 pos);
    void Despawn();
}

interface IDamageable {
    void TakeDamage(float amount);
}

// 2. Clases base, mixins, y clases compartidas / abstractas
shared abstract class BaseEntity {
    string name;
    int id;

    void PrintInfo() {
        // ...
    }
}

mixin class MovableMixin {
    Vector3 position;
    Vector3 velocity;
    
    void Move(float deltaTime) {
        position.x += velocity.x * deltaTime;
        position.y += velocity.y * deltaTime;
        position.z += velocity.z * deltaTime;
    }
}

// Implementamos herencia múltiple combinando herencia simple + mixin + interfaces
class Player : BaseEntity, MovableMixin, IEntity, IDamageable {
    float health = 100.0f;

    Player() {
        name = "Hero";
    }

    void Spawn(Vector3 pos) {
        position = pos;
    }

    void Despawn() {}

    void TakeDamage(float amount) {
        health -= amount;
    }
}

class Vector3 {
    float x, y, z;
    Vector3() {}
    Vector3(float _x, float _y, float _z) { x = _x; y = _y; z = _z; }
}

// Función principal para probar hovers de arreglos y variables `auto`
void Main() {
    // 3. Prueba de Auto
    float maxHealth = 200.0f;
    auto hp = maxHealth;           // Hover en 'hp': float

    Player p1;
    auto mainPlayer = p1;          // Hover en 'mainPlayer': Player

    auto callback = function() {   // Hover en 'callback': function
        // Lógica anónima
    };

    // 4. Pruebas de Arreglos (Arrays)
    array<int> scores = {100, 200, 300};     // Hover en 'scores': array<int>
    array<array<array<int>>> grid3D;         // Hover en 'grid3D': array<array<array<int>>>
    
    array<Player> team;                      // Hover en 'team': array<Player>

    auto teamSize = team.length();           // Hover en 'teamSize' podría no inferirse si .length() no retorna explícitamente en el AST, pero 'team' y 'p1' sí deben funcionar.
}
