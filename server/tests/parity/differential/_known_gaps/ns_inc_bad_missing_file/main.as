// Known gap by design: the compiler rejects a missing include, but the server reports it as warning W112
// because the host (not the compiler) decides the include search paths, so the file may exist at run time.
// @expect: reject
#include "does_not_exist.as"
void main() {}
