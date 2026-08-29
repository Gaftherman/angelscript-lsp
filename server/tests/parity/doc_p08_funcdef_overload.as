// LAMBDA-05. Taking the address of an overloaded function for a funcdef is not ambiguous: the
// funcdef's own signature selects the overload.
funcdef void ActionInt(int);
void Log(string msg) {}
void Log(int v) {}
void main() { ActionInt@ a = @Log; }
