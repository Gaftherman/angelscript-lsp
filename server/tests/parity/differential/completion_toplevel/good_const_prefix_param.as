// @expect: accept
// @complete+ Thorn
// @complete+ ThornBush
// @complete- ThornSpike
class Thorn {}
class ThornBush {}
namespace Briar { class ThornSpike {} }
void inspect(const Th/*$complete*/ &in val) {}
