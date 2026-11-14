// @expect: accept
// @complete+ WindBase
// @complete+ WindRoot
// @complete- WindGust
class WindBase {}
class WindRoot {}
namespace Sky { class WindGust {} }
class Willow : Wi/*$complete*/ {}
