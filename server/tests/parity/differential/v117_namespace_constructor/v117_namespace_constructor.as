// @expect: accept
class Obj {}
namespace HZ::FOO {
class CFoo {
  HZ::List g_List;
  void dox() {
    g_List.LPush(::HZ::List::Node(@Obj()));
  }
}
}
namespace HZ {
namespace List {
funcdef void FreeNode(Obj@);
funcdef bool MatchNode(Obj@, Obj@);
final class Node {
  Node(Obj@ value) { @m_pValue = value; }
  Node@ m_pPrev;
  Node@ m_pNext;
  Obj@ m_pValue;
}
}
final class List {
  List() {}
  List::Node@ LPush(List::Node@ pNode) { return pNode; }
  List::Node@ m_pTail;
}
}
