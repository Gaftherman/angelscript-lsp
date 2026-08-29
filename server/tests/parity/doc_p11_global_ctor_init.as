// PARSER-06. `Type name(args);` at file scope is a variable with a constructor call, not a
// function declaration.
class G { int a; G(int x, float y) { a = x; } }
const G g1(25, .30f);
