// PARSER-12. The declaration modifiers are contextual, so they are ordinary identifiers anywhere a
// modifier cannot appear.
void main() { int property = 10; int override = 20; int final = 30; int explicit = 40;
              print("" + property + override + final + explicit); }
