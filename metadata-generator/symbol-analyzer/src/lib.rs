use std::collections::BTreeSet;
use std::path::Path;

use oxc_allocator::Allocator;
use oxc_ast::ast::{
    Argument, CallExpression, ComputedMemberExpression, Expression, IdentifierReference,
    StaticMemberExpression,
};
use oxc_ast_visit::{Visit, walk};
use oxc_parser::Parser;
use oxc_semantic::SemanticBuilder;
use oxc_span::SourceType;

#[derive(Debug, Default)]
pub struct Analysis {
    pub symbols: BTreeSet<String>,
    pub diagnostics: usize,
    pub bailed_out: bool,
}

#[derive(Default)]
struct GlobalPropertyCollector {
    symbols: BTreeSet<String>,
    dynamic_access: bool,
    global_object_references: usize,
    handled_global_object_references: usize,
}

fn is_global_object(expression: &Expression<'_>) -> bool {
    matches!(
        expression,
        Expression::Identifier(identifier)
            if matches!(identifier.name.as_str(), "globalThis" | "global")
    )
}

impl<'a> Visit<'a> for GlobalPropertyCollector {
    fn visit_identifier_reference(&mut self, identifier: &IdentifierReference<'a>) {
        if matches!(identifier.name.as_str(), "globalThis" | "global") {
            self.global_object_references += 1;
        }
        walk::walk_identifier_reference(self, identifier);
    }

    fn visit_static_member_expression(&mut self, expression: &StaticMemberExpression<'a>) {
        if is_global_object(&expression.object) {
            self.handled_global_object_references += 1;
            self.symbols.insert(expression.property.name.to_string());
            if matches!(expression.property.name.as_str(), "globalThis" | "global") {
                self.dynamic_access = true;
            }
        }
        walk::walk_static_member_expression(self, expression);
    }

    fn visit_computed_member_expression(&mut self, expression: &ComputedMemberExpression<'a>) {
        if is_global_object(&expression.object) {
            self.handled_global_object_references += 1;
            if let Expression::StringLiteral(property) = &expression.expression {
                self.symbols.insert(property.value.to_string());
            } else {
                // A computed global property cannot be resolved statically. Keep all
                // metadata rather than risk a false negative.
                self.dynamic_access = true;
            }
        }
        walk::walk_computed_member_expression(self, expression);
    }

    fn visit_call_expression(&mut self, expression: &CallExpression<'a>) {
        let is_dynamic_native_lookup = matches!(
            &expression.callee,
            Expression::Identifier(identifier)
                if matches!(
                    identifier.name.as_str(),
                    "NSClassFromString"
                        | "NSProtocolFromString"
                        | "objc_getClass"
                        | "objc_lookUpClass"
                        | "objc_getProtocol"
                )
        ) || matches!(
            &expression.callee,
            Expression::StaticMemberExpression(member)
                if is_global_object(&member.object)
                    && matches!(
                        member.property.name.as_str(),
                        "NSClassFromString"
                            | "NSProtocolFromString"
                            | "objc_getClass"
                            | "objc_lookUpClass"
                            | "objc_getProtocol"
                    )
        );
        if is_dynamic_native_lookup {
            match expression.arguments.first() {
                Some(Argument::StringLiteral(name)) => {
                    self.symbols.insert(name.value.to_string());
                }
                _ => self.dynamic_access = true,
            }
        }
        walk::walk_call_expression(self, expression);
    }
}

pub fn analyze_source(path: &Path, source: &str) -> Analysis {
    let source_type = SourceType::from_path(path).unwrap_or_else(|_| SourceType::unambiguous());
    let allocator = Allocator::default();
    let parsed = Parser::new(&allocator, source, source_type).parse();

    if parsed.panicked || !parsed.diagnostics.is_empty() {
        return Analysis {
            diagnostics: parsed.diagnostics.len().max(1),
            bailed_out: true,
            ..Analysis::default()
        };
    }

    let mut global_properties = GlobalPropertyCollector::default();
    global_properties.visit_program(&parsed.program);
    if global_properties.global_object_references
        != global_properties.handled_global_object_references
    {
        // Aliasing, destructuring, or passing the global object to another
        // function can hide native symbol access from this local AST pass.
        global_properties.dynamic_access = true;
    }

    let semantic = SemanticBuilder::new().build(&parsed.program);
    if !semantic.diagnostics.is_empty() {
        return Analysis {
            diagnostics: semantic.diagnostics.len(),
            bailed_out: true,
            ..Analysis::default()
        };
    }

    let mut symbols: BTreeSet<String> = semantic
        .semantic
        .scoping()
        .root_unresolved_references()
        .keys()
        .map(ToString::to_string)
        .collect();
    let uses_dynamic_code = symbols.contains("eval") || symbols.contains("Function");
    symbols.extend(global_properties.symbols);

    Analysis {
        symbols,
        diagnostics: 0,
        bailed_out: global_properties.dynamic_access || uses_dynamic_code,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn collects_only_unbound_runtime_symbols() {
        let result = analyze_source(
            Path::new("bundle.js"),
            "const local = 1; new UIView(); CGRectMake(0, 0, local, 1);",
        );

        assert!(!result.bailed_out);
        assert!(result.symbols.contains("UIView"));
        assert!(result.symbols.contains("CGRectMake"));
        assert!(!result.symbols.contains("local"));
    }

    #[test]
    fn bails_out_when_the_bundle_cannot_be_parsed() {
        let result = analyze_source(Path::new("bundle.js"), "function {");
        assert!(result.bailed_out);
        assert!(result.diagnostics > 0);
    }

    #[test]
    fn collects_static_global_properties_and_fails_open_for_dynamic_access() {
        let static_result = analyze_source(
            Path::new("bundle.js"),
            "new globalThis.UIView(); global['CGRectMake'](0, 0, 1, 1);",
        );
        assert!(!static_result.bailed_out);
        assert!(static_result.symbols.contains("UIView"));
        assert!(static_result.symbols.contains("CGRectMake"));

        let dynamic_result = analyze_source(Path::new("bundle.js"), "globalThis[className]");
        assert!(dynamic_result.bailed_out);

        let aliased_result = analyze_source(
            Path::new("bundle.js"),
            "const nativeGlobal = globalThis; new nativeGlobal.UIView();",
        );
        assert!(aliased_result.bailed_out);

        let destructured_result =
            analyze_source(Path::new("bundle.js"), "const { UIView } = globalThis;");
        assert!(destructured_result.bailed_out);
    }

    #[test]
    fn extracts_literal_native_lookups_and_fails_open_for_dynamic_names() {
        let literal = analyze_source(
            Path::new("bundle.js"),
            "NSClassFromString('NSView'); objc_getProtocol('NSDraggingDestination');",
        );
        assert!(!literal.bailed_out);
        assert!(literal.symbols.contains("NSView"));
        assert!(literal.symbols.contains("NSDraggingDestination"));

        let global_literal = analyze_source(
            Path::new("bundle.js"),
            "globalThis.NSClassFromString('NSWindow')",
        );
        assert!(!global_literal.bailed_out);
        assert!(global_literal.symbols.contains("NSWindow"));

        let dynamic = analyze_source(Path::new("bundle.js"), "NSClassFromString(className)");
        assert!(dynamic.bailed_out);
    }
}
