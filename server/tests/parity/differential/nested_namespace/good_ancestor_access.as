// @expect: accept
namespace A { void top() {} namespace B { void inner() { top(); } } }
