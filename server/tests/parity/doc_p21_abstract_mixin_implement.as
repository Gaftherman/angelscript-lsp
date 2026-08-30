// The accepting side of doc_r08 and doc_r23, which is what keeps the tightened rule from becoming
// a false positive. An abstract class and a mixin may both name an interface; what neither may do
// is leave it unimplemented. Implemented, both compile, and so does a class deriving from either.
interface IThink
{
    void Think();
}

abstract class Partial : IThink
{
    void Think() {}
}

class Concrete : Partial
{
}

mixin class Helper : IThink
{
    void Think() {}
}

class Agent : Helper
{
}
