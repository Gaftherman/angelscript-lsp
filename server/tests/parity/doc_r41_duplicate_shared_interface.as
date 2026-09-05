// The same shared interface declared twice, on a host that does not forgive it.
//
//     angelscript_oracle doc_r41_duplicate_shared_interface.as
//         ERROR (13, 18): Name conflict. 'IDupShared' is an interface.
//
// The engine's default rejects this, and so do we - through the ordinary duplicate-declaration rule
// rather than one of its own. What made it worth a fixture is the OTHER direction: a host that sets
// asEP_IGNORE_DUPLICATE_SHARED_INTF accepts it, and this analyzer was reporting an error there,
// which is a false positive on code that compiles. Measured both ways, and the guard that closes it
// is deliberately narrow - a duplicate PLAIN interface is rejected under both settings, so only
// `shared` on both declarations is what the property forgives.
shared interface IDupShared { void Run(); }
shared interface IDupShared { void Run(); }

class DupSharedUser : IDupShared
{
    void Run() {}
}
