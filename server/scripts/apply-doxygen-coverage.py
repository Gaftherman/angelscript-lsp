#!/usr/bin/env python3
"""
Automated Doxygen & [REFACTOR-CANDIDATE] Annotation Generator.
Applies 100% English Javadoc-style Doxygen comments and mandatory
@note [REFACTOR-CANDIDATE] metadata to all candidate structs and functions
identified in docs/REDUNDANCY_REPORT.json.
"""

import collections
import json
import re
import sys
from pathlib import Path

import tree_sitter
import tree_sitter_cpp

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SERVER_SRC = REPO_ROOT / "server" / "src"
REPORT_PATH = REPO_ROOT / "docs" / "REDUNDANCY_REPORT.json"

CPP_LANGUAGE = tree_sitter.Language(tree_sitter_cpp.language())
PARSER = tree_sitter.Parser(CPP_LANGUAGE)

MODIFIERS = (
    "[[", "template", "inline", "constexpr", "explicit", "static",
    "virtual", "friend", "extern", "const", "override", "final"
)


def camel_to_sentence(name: str) -> str:
    """Converts CamelCase or snake_case to a readable English sentence."""
    name = name.lstrip("*& ")
    # Strip common prefixes
    for prefix in ("m_", "s_", "k_"):
        if name.startswith(prefix):
            name = name[len(prefix):]
            break
    words = re.findall(r"[A-Z]?[a-z0-9]+|[A-Z]+(?=[A-Z][a-z0-9]|\b)", name)
    if not words:
        return f"Operates on {name}."
    sentence = " ".join(words).lower()
    return sentence[0].upper() + sentence[1:] + "."


PARAM_DESCRIPTIONS = {
    "ctx": "Diagnostic collection context.",
    "diag": "Diagnostic collection context.",
    "diags": "Diagnostic output list.",
    "request": "Feature or checker request parameters.",
    "req": "Feature or checker request parameters.",
    "table": "Symbol table to query.",
    "symbolTable": "Symbol table to query.",
    "sourceCode": "Script source code view.",
    "node": "AST node to inspect.",
    "root": "Root AST node.",
    "sym": "Symbol descriptor.",
    "symbol": "Symbol descriptor.",
    "sig": "Signature descriptor.",
    "scope": "Enclosing lexical scope.",
    "name": "Target identifier name.",
    "type": "Semantic type name or descriptor.",
    "typeName": "Semantic type name.",
    "rule": "Engine rule configuration.",
    "cache": "Expression type cache.",
    "index": "Node or symbol index.",
    "tokens": "Token array to populate or inspect.",
}


def get_param_desc(pname: str) -> str:
    if pname in PARAM_DESCRIPTIONS:
        return PARAM_DESCRIPTIONS[pname]
    for key, desc in PARAM_DESCRIPTIONS.items():
        if pname.lower().startswith(key.lower()) or pname.lower().endswith(key.lower()):
            return desc
    return f"Parameter {pname}."


def extract_params_and_return(func_node) -> tuple[list[tuple[str, str]], str]:
    """
    Extracts parameter list [(param_name, direction)] and return type string.
    """
    # Find return type
    type_node = func_node.child_by_field_name("type")
    ret_type = type_node.text.decode("utf-8", "ignore").strip() if type_node else ""

    # Find parameter list
    params = []
    param_list = None
    stack = [func_node]
    while stack:
        n = stack.pop()
        if n.type == "parameter_list":
            param_list = n
            break
        # Don't descend into child function bodies or lambdas
        if n != func_node and n.type in ("function_definition", "compound_statement"):
            continue
        for i in range(n.child_count - 1, -1, -1):
            stack.append(n.child(i))

    if param_list:
        for c in param_list.children:
            if c.type == "parameter_declaration":
                c_text = c.text.decode("utf-8", "ignore").strip()
                t_node = c.child_by_field_name("type")
                t_str = t_node.text.decode("utf-8", "ignore").strip() if t_node else ""
                d_node = c.child_by_field_name("declarator")
                if d_node:
                    raw_name = d_node.text.decode("utf-8", "ignore").strip()
                    # Clean up pointer/ref and default arguments
                    pname = raw_name.split("=")[0].strip().lstrip("&* ").strip()
                    if pname:
                        is_const = "const" in c_text
                        is_mut_ref = ("&" in raw_name or "*" in raw_name or "&" in t_str or "*" in t_str) and not is_const
                        direction = "[in,out]" if is_mut_ref else "[in]"
                        params.append((pname, direction))
    return params, ret_type


def find_doxygen_span(lines: list[str], start_line_1_based: int) -> tuple[int, int] | None:
    """Returns (start_idx, end_idx) 0-based lines of preceding doxygen comment, or None."""
    idx = start_line_1_based - 2
    while idx >= 0:
        line = lines[idx].strip()
        if not line:
            idx -= 1
            continue
        if any(line.startswith(m) for m in MODIFIERS):
            idx -= 1
            continue
        if line.endswith("*/"):
            c_idx = idx
            while c_idx >= 0:
                c_line = lines[c_idx].strip()
                if "/**" in c_line:
                    return (c_idx, idx)
                if "/*" in c_line:
                    return None
                c_idx -= 1
            return None
        return None
    return None


def find_insertion_index(lines: list[str], start_line_1_based: int) -> int:
    """Finds the topmost 0-based line index above modifiers where Doxygen should be inserted."""
    idx = start_line_1_based - 2
    top_idx = start_line_1_based - 1
    while idx >= 0:
        line = lines[idx].strip()
        if not line:
            # Check if there are modifiers before empty lines
            peek_idx = idx - 1
            while peek_idx >= 0 and not lines[peek_idx].strip():
                peek_idx -= 1
            if peek_idx >= 0 and any(lines[peek_idx].strip().startswith(m) for m in MODIFIERS):
                idx = peek_idx
                continue
            break
        if any(line.startswith(m) for m in MODIFIERS):
            top_idx = idx
            idx -= 1
            continue
        break
    return top_idx


def augment_doxygen_block(lines: list[str], start_c: int, end_c: int, note: str):
    """Augments an existing Doxygen comment with @note [REFACTOR-CANDIDATE]."""
    existing_block = "\n".join(lines[start_c:end_c + 1])
    if "@note [REFACTOR-CANDIDATE]" in existing_block:
        return

    indent = " " * (len(lines[start_c]) - len(lines[start_c].lstrip()))
    if start_c == end_c:
        # Single-line /** ... */ -> unpack to multi-line
        raw = lines[start_c].strip()
        inner = raw[3:-2].strip().lstrip("* ").strip()
        new_block = [
            f"{indent}/**",
            f"{indent} * {inner}",
            f"{indent} * @note [REFACTOR-CANDIDATE] {note}",
            f"{indent} */"
        ]
        lines[start_c:start_c + 1] = new_block
    else:
        end_line = lines[end_c].rstrip()
        if end_line.strip() == "*/":
            lines.insert(end_c, f"{indent} * @note [REFACTOR-CANDIDATE] {note}")
        else:
            # '*/' on same line as text
            content_part = end_line.rsplit("*/", 1)[0].rstrip()
            lines[end_c] = content_part
            lines.insert(end_c + 1, f"{indent} * @note [REFACTOR-CANDIDATE] {note}")
            lines.insert(end_c + 2, f"{indent} */")


def build_new_doxygen_block(indent: str, name: str, kind: str, note: str, params: list[tuple[str, str]], ret_type: str) -> list[str]:
    """Constructs a complete, compliant Javadoc Doxygen block."""
    doc = [f"{indent}/**"]
    if kind == "struct":
        doc.append(f"{indent} * @brief Represents {camel_to_sentence(name)}")
    else:
        doc.append(f"{indent} * @brief {camel_to_sentence(name)}")

    if kind == "function":
        for pname, direction in params:
            doc.append(f"{indent} * @param{direction} {pname} {get_param_desc(pname)}")
        if ret_type and ret_type != "void":
            if ret_type == "bool":
                doc.append(f"{indent} * @return True if condition met; otherwise false.")
            elif "optional" in ret_type:
                doc.append(f"{indent} * @return Found item if present; std::nullopt otherwise.")
            else:
                doc.append(f"{indent} * @return Result of the operation.")

    doc.append(f"{indent} * @note [REFACTOR-CANDIDATE] {note}")
    doc.append(f"{indent} */")
    return doc


def collect_annotations(data: dict) -> dict[str, dict[int, dict]]:
    """
    Collects and clusters annotations for all candidate targets across all 4 categories.
    Returns: file_path -> { start_line: annotation_info }
    """
    annotations_by_file = collections.defaultdict(dict)

    # 1. Local structs in .cpp files
    cluster_by_struct = {}
    for cl in data.get("localCppStructs", {}).get("clusters", []):
        cname = cl["clusterName"]
        ctarget = cl["canonicalTargetHeader"]
        for st in cl["structs"]:
            key = (st["file"], st["structName"])
            cluster_by_struct[key] = (cname, ctarget)

    for st in data.get("localCppStructs", {}).get("structs", []):
        f = st["file"]
        sname = st["structName"]
        sline = st["startLine"]
        cinfo = cluster_by_struct.get((f, sname))
        if cinfo:
            note = f"Clustered under {cinfo[0]}. Candidate for unification in {cinfo[1]}."
        else:
            note = "Local helper structure candidate for structural deduplication."
        annotations_by_file[f][sline] = {
            "name": sname,
            "kind": "struct",
            "notes": [note],
        }

    # 2. Similar functions (ALL pairs)
    for m in data.get("similarFunctionsMatrix", {}).get("matrix", []):
        fA = m["funcA"]
        fB = m["funcB"]
        pct = int(round(m["similarityScore"] * 100))
        noteA = f"Structural overlap ({pct}%) with {fB['name']} in {fB['file']}. Candidate for unification in {m['unificationPlan']}."
        noteB = f"Structural overlap ({pct}%) with {fA['name']} in {fA['file']}. Candidate for unification in {m['unificationPlan']}."

        for item, note in ((fA, noteA), (fB, noteB)):
            fpath = item["file"]
            sline = item["startLine"]
            if sline in annotations_by_file[fpath]:
                if note not in annotations_by_file[fpath][sline]["notes"]:
                    annotations_by_file[fpath][sline]["notes"].append(note)
            else:
                annotations_by_file[fpath][sline] = {
                    "name": item["name"],
                    "kind": "function",
                    "notes": [note],
                }

    # 3. Pass 1 flag reparsing occurrences: map to enclosing function declaration
    for p in data.get("pass1FlagReparsing", {}).get("occurrences", []):
        fpath = p["file"]
        pline = p["line"]
        full_p = REPO_ROOT / fpath.replace("/", "\\")
        if not full_p.is_file():
            full_p = REPO_ROOT / fpath
        if not full_p.is_file():
            continue
        try:
            tree = PARSER.parse(full_p.read_bytes())
        except Exception:
            continue
        stack = [tree.root_node]
        enclosing_line = None
        enclosing_name = ""
        while stack:
            n = stack.pop()
            if n.type == "function_definition":
                sl = n.start_point[0] + 1
                el = n.end_point[0] + 1
                if sl <= pline <= el:
                    decl = n.child_by_field_name("declarator")
                    if decl:
                        enclosing_line = sl
                        enclosing_name = decl.text.decode("utf-8", "ignore").split("(")[0].strip()
                    break
            for i in range(n.child_count - 1, -1, -1):
                stack.append(n.child(i))

        if enclosing_line:
            note = f"Manually re-evaluates AST for {p['flagKind']}; candidate for direct Pass 1 flag read via {p['canonicalPass1Source']}."
            if enclosing_line in annotations_by_file[fpath]:
                if note not in annotations_by_file[fpath][enclosing_line]["notes"]:
                    annotations_by_file[fpath][enclosing_line]["notes"].append(note)
            else:
                annotations_by_file[fpath][enclosing_line] = {
                    "name": enclosing_name,
                    "kind": "function",
                    "notes": [note],
                }

    # 4. Core memory layout structs (SymbolTable.h, ScopeTree.h, NodeIndex.h)
    core_headers = {
        "server/src/analysis/SymbolTable.h": {
            "SymbolModifiers": "Target for memory layout compaction (pack 14 bools into uint16_t in Phase R1).",
            "ParameterInformation": "Target for memory layout compaction (eliminate rawText, pack 9 bools into uint16_t qualifiers in Phase R1).",
            "VariableSignature": "Target for memory layout compaction (pack 19 bools into uint32_t flags in Phase R1).",
            "FunctionSignature": "Target for memory layout compaction (pack return qualifiers in Phase R1).",
            "FuncdefSignature": "Target for memory layout compaction in Phase R1.",
            "ClassSignature": "Target for memory layout compaction (pack bool flags in Phase R1).",
            "InterfaceSignature": "Target for memory layout compaction in Phase R1.",
            "TypedefSignature": "Target for memory layout compaction in Phase R1.",
            "EnumSignature": "Target for memory layout compaction in Phase R1.",
            "Symbol": "Target for memory layout compaction (compact variant, pack bool flags in Phase R1).",
        },
        "server/src/analysis/ScopeTree.h": {
            "LocalDefinition": "Target for memory layout compaction (pack flags, bundle positions into SourceRange in Phase R1).",
            "Scope": "Target for memory layout compaction (pack ScopeKind and isFunctionScope into uint8_t in Phase R1).",
        },
        "server/src/analysis/NodeIndex.h": {
            "IndexedContainerSpan": "Target for memory layout compaction (pack isMixin with symbol in Phase R1).",
            "NodeIndex": "Target for memory layout compaction (eliminate struct padding in Phase R1).",
        },
    }

    for ch, struct_notes in core_headers.items():
        full_p = REPO_ROOT / ch.replace("/", "\\")
        if not full_p.is_file():
            full_p = REPO_ROOT / ch
        if not full_p.is_file():
            continue
        try:
            tree = PARSER.parse(full_p.read_bytes())
        except Exception:
            continue
        stack = [tree.root_node]
        while stack:
            n = stack.pop()
            if n.type in ("struct_specifier", "class_specifier"):
                name_node = n.child_by_field_name("name")
                if name_node:
                    nm = name_node.text.decode("utf-8", "ignore")
                    if nm in struct_notes:
                        sl = n.start_point[0] + 1
                        note = struct_notes[nm]
                        if sl in annotations_by_file[ch]:
                            if note not in annotations_by_file[ch][sl]["notes"]:
                                annotations_by_file[ch][sl]["notes"].append(note)
                        else:
                            annotations_by_file[ch][sl] = {
                                "name": nm,
                                "kind": "struct",
                                "notes": [note],
                            }
            for i in range(n.child_count - 1, -1, -1):
                stack.append(n.child(i))

    return annotations_by_file


def apply_annotations():
    if not REPORT_PATH.is_file():
        print(f"[ERROR] Missing {REPORT_PATH}. Run audit-codebase-redundancy.py first.")
        sys.exit(1)

    data = json.loads(REPORT_PATH.read_text(encoding="utf-8"))
    annotations_by_file = collect_annotations(data)

    total_added = 0
    total_augmented = 0

    for fpath, items in annotations_by_file.items():
        full_p = REPO_ROOT / fpath.replace("/", "\\")
        if not full_p.is_file():
            full_p = REPO_ROOT / fpath
        if not full_p.is_file():
            continue

        try:
            raw_bytes = full_p.read_bytes()
            content = raw_bytes.decode("utf-8")
        except Exception as e:
            print(f"[WARN] Failed to read {fpath}: {e}")
            continue

        lines = content.splitlines()

        # Parse AST to inspect parameters and return types
        try:
            tree = PARSER.parse(raw_bytes)
        except Exception:
            tree = None

        ast_info_by_line = {}
        if tree:
            stack = [tree.root_node]
            while stack:
                n = stack.pop()
                if n.type == "function_definition":
                    sl = n.start_point[0] + 1
                    decl = n.child_by_field_name("declarator")
                    name = decl.text.decode("utf-8", "ignore").split("(")[0].strip() if decl else "Function"
                    params, ret_type = extract_params_and_return(n)
                    ast_info_by_line[sl] = {
                        "name": name,
                        "kind": "function",
                        "params": params,
                        "ret_type": ret_type,
                    }
                elif n.type in ("struct_specifier", "class_specifier"):
                    sl = n.start_point[0] + 1
                    name_node = n.child_by_field_name("name")
                    name = name_node.text.decode("utf-8", "ignore") if name_node else "Struct"
                    ast_info_by_line[sl] = {
                        "name": name,
                        "kind": "struct",
                        "params": [],
                        "ret_type": "",
                    }
                for i in range(n.child_count - 1, -1, -1):
                    stack.append(n.child(i))

        # Process lines in reverse order (bottom to top)
        sorted_lines = sorted(items.keys(), reverse=True)

        for line_num in sorted_lines:
            ann = items[line_num]
            combined_note = " ".join(ann["notes"])
            dox_span = find_doxygen_span(lines, line_num)

            if dox_span is not None:
                start_c, end_c = dox_span
                augment_doxygen_block(lines, start_c, end_c, combined_note)
                total_augmented += 1
            else:
                # Find insertion position
                top_idx = find_insertion_index(lines, line_num)
                decl_line = lines[top_idx] if 0 <= top_idx < len(lines) else lines[line_num - 1]
                indent = " " * (len(decl_line) - len(decl_line.lstrip()))

                info = ast_info_by_line.get(line_num, {})
                name = info.get("name") or ann["name"]
                kind = info.get("kind") or ann["kind"]
                params = info.get("params", [])
                ret_type = info.get("ret_type", "")

                doc_lines = build_new_doxygen_block(
                    indent=indent,
                    name=name,
                    kind=kind,
                    note=combined_note,
                    params=params,
                    ret_type=ret_type,
                )

                for offset, dline in enumerate(doc_lines):
                    lines.insert(top_idx + offset, dline)
                total_added += 1

        full_p.write_text("\n".join(lines) + "\n", encoding="utf-8")

    fixed_directions = fix_const_param_doxygen_directions()
    print(f"[DONE] Created {total_added} new Doxygen blocks and augmented {total_augmented} existing blocks.")
    if fixed_directions:
        print(f"[DONE] Corrected {fixed_directions} invalid @param[in,out] tags on const parameters to @param[in].")


def fix_const_param_doxygen_directions() -> int:
    """Scans all server/src/ files and replaces @param[in,out] with @param[in] for const parameters."""
    fixed = 0
    all_files = sorted(SERVER_SRC.rglob("*.[ch]*"))
    for f in all_files:
        try:
            text = f.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        if "@param[in,out]" not in text:
            continue
        tree = PARSER.parse(text.encode("utf-8"))
        lines = text.splitlines()
        stack = [tree.root_node]
        file_modified = False

        while stack:
            n = stack.pop()
            if n.type in ("function_definition", "declaration"):
                sl = n.start_point[0]
                param_list = None
                p_stack = [n]
                while p_stack:
                    pn = p_stack.pop()
                    if pn.type == "parameter_list":
                        param_list = pn
                        break
                    if pn != n and pn.type in ("function_definition", "compound_statement"):
                        continue
                    for i in range(pn.child_count - 1, -1, -1):
                        p_stack.append(pn.child(i))

                if param_list:
                    const_params = set()
                    for c in param_list.children:
                        if c.type == "parameter_declaration":
                            c_text = c.text.decode("utf-8", "ignore")
                            d_node = c.child_by_field_name("declarator")
                            if d_node:
                                raw_name = d_node.text.decode("utf-8", "ignore").strip()
                                pname = raw_name.split("=")[0].strip().lstrip("&* ").strip()
                                if pname and "const" in c_text:
                                    const_params.add(pname)

                    if const_params:
                        idx = sl - 1
                        while idx >= 0:
                            l = lines[idx].strip()
                            if not l:
                                idx -= 1
                                continue
                            if any(l.startswith(m) for m in MODIFIERS):
                                idx -= 1
                                continue
                            if l.endswith("*/"):
                                c_idx = idx
                                while c_idx >= 0:
                                    for cp in const_params:
                                        pat = re.compile(r"(@param\[)in,out(\]\s+" + re.escape(cp) + r"\b)")
                                        if pat.search(lines[c_idx]):
                                            lines[c_idx] = pat.sub(r"\1in\2", lines[c_idx])
                                            fixed += 1
                                            file_modified = True
                                    if "/**" in lines[c_idx]:
                                        break
                                    c_idx -= 1
                                break
                            break
            for i in range(n.child_count - 1, -1, -1):
                stack.append(n.child(i))

        if file_modified:
            f.write_text("\n".join(lines) + "\n", encoding="utf-8")

    return fixed


if __name__ == "__main__":
    apply_annotations()
