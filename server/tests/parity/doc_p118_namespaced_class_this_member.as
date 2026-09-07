// A method reached through `this`, in a class that lives inside a namespace of the same name.
//
//     angelscript_oracle doc_p118_namespaced_class_this_member.as
//         (accepted, no output)
//
// A class in a namespace is collected as `NS::Class` and its methods as `NS::Class::member`. Typing
// `this` by the class's simple name sent the lookup to `Class::member`, which nothing declares: 27
// errors on one real Sven Co-op file, on every `this.SomeCallback` in it.
//
// The namespace shares the class's name on purpose. That is what made the bug visible rather than
// silent - with no namespace of that name the simple name resolves to nothing, the hierarchy is not
// fully visible, and the rule says nothing at all.
funcdef void P118Callback(int slot);

class P118Menu
{
    P118Menu(P118Callback@ cb) {}
}

namespace P118Box
{
    string P118Label = "x";

    final class P118Box
    {
        P118Menu@ m_pMenu;

        void Create() { @m_pMenu = P118Menu( P118Callback( this.MainCallback ) ); }

        private void MainCallback(int slot) {}
    }
}
