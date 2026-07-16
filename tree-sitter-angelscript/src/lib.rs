use tree_sitter_language::LanguageFn;

unsafe extern "C" {
    fn tree_sitter_angelscript() -> *const ();
}

pub const LANGUAGE: LanguageFn = unsafe { LanguageFn::from_raw(tree_sitter_angelscript) };

pub const NODE_TYPES: &str = include_str!("node-types.json");
pub const HIGHLIGHTS_QUERY: &str = include_str!("../queries/highlights.scm");

#[cfg(test)]
mod tests {
    #[test]
    fn test_can_load_grammar() {
        let mut parser = tree_sitter::Parser::new();
        parser
            .set_language(&super::LANGUAGE.into())
            .expect("Error loading AngelScript parser");
    }
}
