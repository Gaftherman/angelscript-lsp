// Known gap: the server accepts `S::C` for a type declared in `namespace H::S`; the compiler rejects it.
// @expect: reject
namespace HavenRealm::SanctuaryGrounds {
    class SacredShrine {}
}
void visitShrine() {
    SanctuaryGrounds::SacredShrine s;
}
