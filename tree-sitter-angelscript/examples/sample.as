// =============================================================================
// Example AngelScript file for tree-sitter testing
// Exercises every major construct recognized by the grammar.
// Validate with:  npx tree-sitter parse examples/sample.as
//                 npx tree-sitter highlight examples/sample.as
// =============================================================================

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------
import void ExternalFunc(int) from "external_module";
import int& GetGlobalRef(const string &in) from "core";

// ---------------------------------------------------------------------------
// Using
// ---------------------------------------------------------------------------
using namespace System;
using namespace Util::Collections;

// ---------------------------------------------------------------------------
// Typedef
// ---------------------------------------------------------------------------
typedef int32 MyInt;
typedef float Real;

// ---------------------------------------------------------------------------
// Funcdef
// ---------------------------------------------------------------------------
funcdef void Callback(int, float);
shared funcdef bool Predicate(const string &in);

// ---------------------------------------------------------------------------
// Enum (basic and shared)
// ---------------------------------------------------------------------------
enum Color {
    RED = 0,
    GREEN = 1,
    BLUE = 2,
}

shared enum LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR
}

// ---------------------------------------------------------------------------
// Interface
// ---------------------------------------------------------------------------
interface IUpdatable {
    void Update(float dt);
    bool get_IsActive() const;
}

interface ISerializable : IUpdatable {
    string Serialize();
}

// ---------------------------------------------------------------------------
// Class (shared, abstract, with base list and all member kinds)
// ---------------------------------------------------------------------------
shared abstract class BaseEntity : IUpdatable, ISerializable {
    private string name;
    protected int health = 100;

    // Constructor
    BaseEntity() {}

    // Parameterised constructor with default value
    BaseEntity(const string &in n, int h = 100) {
        name = n;
        health = h;
    }

    // Destructor
    ~BaseEntity() {}

    // Virtual property with get/set accessors
    string Name {
        get const { return name; }
        set { name = value; }
    }

    // Interface method implementation
    void Update(float dt) override {
        health = cast<int>(health - dt * 10.0f);

        if (health <= 0) {
            OnDeath();
        } else if (health < 50) {
            OnLowHealth();
        }
    }

    bool get_IsActive() const property {
        return health > 0;
    }

    string Serialize() {
        return name;
    }

    private void OnDeath() {
        switch (health) {
            case 0:
                break;
            default:
                return;
        }
    }

    private void OnLowHealth() {}
}

// Concrete class
final class Player : BaseEntity {
    int score = 0;

    Player(const string &in n) {
        super(n, 200);
    }

    void AddScore(int pts) {
        score += pts;
    }
}

// ---------------------------------------------------------------------------
// Mixin
// ---------------------------------------------------------------------------
mixin class Loggable {
    void Log(const string &in msg) {}
}

// ---------------------------------------------------------------------------
// Namespace with various constructs inside
// ---------------------------------------------------------------------------
namespace Game {
    // Nested namespace
    namespace Internal {
        int helperValue = 42;
    }

    void Init() {
        // Variable declarations
        int count = 0;
        float delta = 0.016f;
        bool running = true;
        string label = "test";
        const int MAX = 100;

        // For loop
        for (int i = 0; i < 10; i++) {
            count += i;
        }

        // Empty for loop parts
        for (;;) {
            break;
        }

        // While
        while (count > 0) {
            count--;
        }

        // Do-while
        do {
            count++;
        } while (count < 5);

        // Foreach
        int[] values = {1, 2, 3, 4, 5};
        foreach (int v : values) {
            count += v;
        }

        // Switch
        switch (count) {
            case 0:
            case 1:
                count = 10;
                break;
            case 2:
                break;
            default:
                count = -1;
        }

        // Try-catch
        try {
            count = count / 0;
        } catch {
            count = 0;
        }

        // If-else chain
        if (count == 0) {
            count = 1;
        } else if (count == 1) {
            count = 2;
        } else {
            count = 3;
        }
    }

    // ---------------------------------------------------------------------------
    // Expressions
    // ---------------------------------------------------------------------------
    void ExpressionShowcase() {
        // Arithmetic operators
        int a = 2 ** 10;
        int b = 10 + 3 - 1;
        int c = b * 2 / 4 % 3;

        // Comparison
        bool lt = a < b;
        bool le = a <= b;
        bool gt = a > b;
        bool ge = a >= b;
        bool eq = a == b;
        bool ne = a != b;

        // Logical operators (symbol and keyword forms)
        bool l1 = true && false;
        bool l2 = true || false;
        bool l3 = true ^^ false;
        bool l4 = true and false;
        bool l5 = true or false;
        bool l6 = true xor false;
        bool l7 = !true;

        // Bitwise operators
        int bw1 = a & 0xFF;
        int bw2 = a | 0x0F;
        int bw3 = a ^ 0xAA;
        int bw4 = ~a;
        int bw5 = a << 2;
        int bw6 = a >> 2;
        int bw7 = a >>> 1;

        // Assignment operators
        a += 1;
        a -= 1;
        a *= 2;
        a /= 2;
        a %= 3;
        a **= 2;
        a &= 0xFF;
        a |= 0x01;
        a ^= 0xAA;
        a <<= 1;
        a >>= 1;
        a >>>= 1;

        // Increment / Decrement (prefix and postfix)
        ++a;
        --a;
        a++;
        a--;

        // Ternary expression
        int result = (a > 0) ? a : -a;

        // Identity operators
        Player@ p = null;
        bool isNull = p is null;
        bool isNotNull = p !is null;

        // Handle-of operator
        Player@ handle = @p;

        // Cast expression
        int casted = cast<int>(3.14);

        // Lambda expression
        auto fn = function(int x) { return x * 2; };

        // Member access
        Player@ player = Player("Alice");
        int s = player.score;
        player.AddScore(10);

        // Index expression
        int[] arr = {10, 20, 30};
        int first = arr[0];

        // Named arguments in function call
        // (syntax exercised even if not semantically meaningful)

        // Nested expressions
        int nested = (a + b) * (c - 1);

        // Initializer list
        int[] nums = {1, 2, 3};

        // Number literal forms
        int hex = 0xFF;
        int oct = 0o77;
        int bin = 0b1010;
        float flt = 1.5f;
        double dbl = 3.14;
        float sci = 1.0e10f;

        // String literal forms
        string dq = "double quoted";
        string sq = 'single quoted';
        string heredoc = """
            This is a
            heredoc string
        """;
    }
}

// ---------------------------------------------------------------------------
// Top-level function
// ---------------------------------------------------------------------------
void main() {
    Init();
    ExpressionShowcase();
}

// Forward declaration (external)
external shared void NetworkSync();
