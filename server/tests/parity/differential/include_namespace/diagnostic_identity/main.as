// @expect: reject
// @error: main.as as-warn-undeclared-identifier 4:29-4:40
// @error: peer.as as-warn-undeclared-identifier 4:29-4:40
#include "peer.as"
void Main() { /*😀*/ int v = missingMain; }
