// LAMBDA-01. A lambda captures nothing, but a global is not a capture - it is still in scope.
// The compiler rejects a local (`No matching symbol 'local'`) and a member (`'m'`), and accepts
// this.
funcdef void CB();
int g_counter = 5;
void Init() { CB@ cb = function() { g_counter = 100; }; }
