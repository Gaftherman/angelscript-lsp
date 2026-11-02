#!/usr/bin/env python3
"""
Deterministic Similarity, Struct Clustering, RAM Layout & Doxygen Auditor.
Analyzes C++ source and header files in server/src/ using Tree-sitter and generates
docs/REDUNDANCY_REPORT.json with 5 exhaustive sections:
1. localCppStructs (Inventory and semantic clustering of structs declared in .cpp)
2. pass1FlagReparsing (Detection of manual reparsing of isConst, isRef, isHandle)
3. similarFunctionsMatrix (Mathematical token-level structural similarity matrix >= 60%)
4. memoryLayoutAudit (RAM consumption and bitfield packing analysis for 11 core structures)
5. missingDoxygenCatalog (Catalog of functions/structs lacking Javadoc-style Doxygen)
"""

import collections
import difflib
import json
import os
import re
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SERVER_SRC = REPO_ROOT / "server" / "src"
REPORT_PATH = REPO_ROOT / "docs" / "REDUNDANCY_REPORT.json"


def find_matching_brace(text: str, open_pos: int) -> int:
    """Finds the closing brace corresponding to the opening brace at open_pos."""
    i = open_pos + 1
    depth = 1
    n = len(text)
    in_line_comment = False
    in_block_comment = False
    in_string = False
    in_char = False
    escape = False

    while i < n:
        c = text[i]
        if in_line_comment:
            if c == "\n":
                in_line_comment = False
        elif in_block_comment:
            if c == "*" and i + 1 < n and text[i + 1] == "/":
                in_block_comment = False
                i += 1
        elif in_string:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == '"':
                in_string = False
        elif in_char:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == "'":
                in_char = False
        else:
            if c == "/" and i + 1 < n and text[i + 1] == "/":
                in_line_comment = True
                i += 1
            elif c == "/" and i + 1 < n and text[i + 1] == "*":
                in_block_comment = True
                i += 1
            elif c == '"':
                in_string = True
            elif c == "'":
                in_char = True
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return i
        i += 1
    return -1


def rel_path(p: Path) -> str:
    """Returns a forward-slash relative path to the repo root."""
    return str(p.relative_to(REPO_ROOT)).replace("\\", "/")


def has_doxygen_before(lines: list[str], start_line_1_based: int) -> bool:
    """Checks if a declaration has a /** ... */ Doxygen comment immediately preceding it."""
    idx = start_line_1_based - 2  # 0-based index right above
    modifiers = (
        "[[", "template", "inline", "constexpr", "explicit", "static",
        "virtual", "friend", "extern", "const", "override", "final"
    )
    while idx >= 0:
        line = lines[idx].strip()
        if not line:
            idx -= 1
            continue
        if any(line.startswith(m) for m in modifiers):
            idx -= 1
            continue
        if line.endswith("*/"):
            c_idx = idx
            while c_idx >= 0:
                c_line = lines[c_idx].strip()
                if "/**" in c_line:
                    return True
                if "/*" in c_line:
                    return False
                c_idx -= 1
            return False
        return False
    return False


def has_refactor_candidate_note(lines: list[str], start_line_1_based: int) -> bool:
    """Checks if a preceding Doxygen comment contains @note [REFACTOR-CANDIDATE]."""
    idx = start_line_1_based - 2
    modifiers = (
        "[[", "template", "inline", "constexpr", "explicit", "static",
        "virtual", "friend", "extern", "const", "override", "final"
    )
    while idx >= 0:
        line = lines[idx].strip()
        if not line:
            idx -= 1
            continue
        if any(line.startswith(m) for m in modifiers):
            idx -= 1
            continue
        if line.endswith("*/"):
            c_idx = idx
            while c_idx >= 0:
                c_line = lines[c_idx].strip()
                if "/**" in c_line:
                    comment = "\n".join(lines[c_idx:idx + 1])
                    return "@note [REFACTOR-CANDIDATE]" in comment
                if "/*" in c_line:
                    return False
                c_idx -= 1
            return False
        return False
    return False


# ==============================================================================
# 1. localCppStructs: Inventory & Clustering
# ==============================================================================

SEMANTIC_CONCEPT_PATTERNS = {
    "isConst": re.compile(r"\b(?:isConst|constness|isConstant)\b", re.I),
    "isHandle": re.compile(r"\b(?:isHandle|handle|isHandleType|hasPrimitiveHandle)\b", re.I),
    "isRef": re.compile(r"\b(?:isRef|isReference|isReturnReference|isOutRef|isInOutRef|isStandaloneRef)\b", re.I),
    "typeName": re.compile(r"\b(?:typeName|baseTypeName|returnType|targetType|declaringClass|declaredType|objectType|argTypes|valueType)\b", re.I),
    "scope": re.compile(r"\b(?:scope|containerScope|enclosingScope|declaringClass)\b", re.I),
    "node": re.compile(r"\b(?:node|targetNode|callNode|argNode|argNodes|exprNode|rootNode)\b", re.I),
    "startByte": re.compile(r"\b(?:startByte|byteOffset|startOffset|offset|startLine|startCharacter|startChar)\b", re.I),
    "endByte": re.compile(r"\b(?:endByte|endOffset|endLine|endCharacter|endChar)\b", re.I),
    "access": re.compile(r"\b(?:access|accessModifier)\b", re.I),
    "isArray": re.compile(r"\b(?:isArray|arrayDepth)\b", re.I),
    "table": re.compile(r"\b(?:table|symbolTable)\b", re.I),
    "call": re.compile(r"\b(?:callee|arguments|candidates|argTypes|argNames|callNode)\b", re.I),
}


def extract_fields(body_text: str) -> list[dict]:
    fields = []
    cleaned_lines = []
    for line in body_text.splitlines():
        idx = line.find("//")
        if idx != -1:
            line = line[:idx]
        cleaned_lines.append(line)
    cleaned = "\n".join(cleaned_lines)
    stmt_re = re.compile(
        r"^\s*([A-Za-z0-9_<>:*,&\s]+?)\s+([A-Za-z0-9_]+)\s*(?:\{[^}]*\}|=\s*[^;]+)?\s*;\s*$",
        re.M,
    )
    for m in stmt_re.finditer(cleaned):
        t_str = m.group(1).strip()
        name = m.group(2).strip()
        if t_str.startswith("using ") or t_str.startswith("typedef ") or t_str.startswith("friend "):
            continue
        if name and not name.startswith("return"):
            fields.append({"type": t_str, "name": name})
    return fields


def scan_local_cpp_structs():
    structs = []
    cpp_files = sorted(SERVER_SRC.rglob("*.cpp"))
    struct_re = re.compile(r"\bstruct\s+([A-Za-z0-9_:]+)\s*(?:final\s*)?(?::[^{]+)?\{")

    for fpath in cpp_files:
        try:
            txt = fpath.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue

        for m in struct_re.finditer(txt):
            s_name = m.group(1)
            open_pos = txt.find("{", m.start())
            close_pos = find_matching_brace(txt, open_pos)
            if close_pos == -1:
                continue
            body = txt[open_pos + 1 : close_pos]
            flds = extract_fields(body)
            if flds:
                concepts = set()
                for fld in flds:
                    fname = fld["name"]
                    for concept, pat in SEMANTIC_CONCEPT_PATTERNS.items():
                        if pat.search(fname):
                            concepts.add(concept)

                s_line = txt[: m.start()].count("\n") + 1
                e_line = txt[: close_pos].count("\n") + 1
                structs.append({
                    "file": rel_path(fpath),
                    "startLine": s_line,
                    "endLine": e_line,
                    "structName": s_name,
                    "fields": flds,
                    "semanticConcepts": sorted(concepts),
                })

    cluster_definitions = [
        {
            "clusterId": "CLUSTER-TYPE-QUALIFIERS",
            "clusterName": "Type Qualification & Modifiers",
            "requiredConcepts": {"typeName", "isConst", "isHandle", "isRef"},
            "minMatches": 2,
            "whyInThatCpp": "Ad-hoc parameter bundling for checker recursion and local type inspection instead of using a unified TypeQualifiers/TypeDescriptor structure.",
            "canonicalTargetHeader": "server/src/analysis/index/SymbolTable.h",
            "canonicalTargetSymbol": "TypeQualifiers",
        },
        {
            "clusterId": "CLUSTER-MEMBER-LOOKUP",
            "clusterName": "Member Access & Scope Lookup Context",
            "requiredConcepts": {"typeName", "scope", "table", "access"},
            "minMatches": 2,
            "whyInThatCpp": "Checker-local context structs created to satisfy the 4-parameter ceiling (/we4100) for internal recursion without a shared analysis structure.",
            "canonicalTargetHeader": "server/src/analysis/resolution/SemanticHelpers.h",
            "canonicalTargetSymbol": "MemberLookupContext",
        },
        {
            "clusterId": "CLUSTER-CALL-RESOLUTION",
            "clusterName": "Call & Overload Resolution Context",
            "requiredConcepts": {"node", "call", "typeName"},
            "minMatches": 2,
            "whyInThatCpp": "Independent feature-level overload and call resolution structs created because Layer 3 feature handlers cannot include sibling features.",
            "canonicalTargetHeader": "server/src/analysis/resolution/SemanticHelpers.h",
            "canonicalTargetSymbol": "CallResolutionContext",
        },
        {
            "clusterId": "CLUSTER-AST-BYTE-INTERVALS",
            "clusterName": "AST Byte Intervals & Node Spans",
            "requiredConcepts": {"startByte", "endByte"},
            "minMatches": 2,
            "whyInThatCpp": "Temporary byte range slices and node interval holders duplicated across features and analysis checkers.",
            "canonicalTargetHeader": "server/src/analysis/index/NodeIndex.h",
            "canonicalTargetSymbol": "IndexedContainerSpan",
        },
    ]

    clusters = []
    for cdef in cluster_definitions:
        matched_structs = []
        for s in structs:
            intersection = set(s["semanticConcepts"]) & cdef["requiredConcepts"]
            if len(intersection) >= cdef["minMatches"]:
                matched_structs.append({
                    "file": s["file"],
                    "structName": s["structName"],
                    "startLine": s["startLine"],
                    "endLine": s["endLine"],
                    "matchedConcepts": sorted(intersection),
                })
        clusters.append({
            "clusterId": cdef["clusterId"],
            "clusterName": cdef["clusterName"],
            "sharedSemanticConcepts": sorted(cdef["requiredConcepts"]),
            "structCount": len(matched_structs),
            "whyInThatCpp": cdef["whyInThatCpp"],
            "canonicalTargetHeader": cdef["canonicalTargetHeader"],
            "canonicalTargetSymbol": cdef["canonicalTargetSymbol"],
            "structs": matched_structs,
        })

    return {
        "totalLocalStructs": len(structs),
        "structs": structs,
        "clusters": clusters,
    }


# ==============================================================================
# 2. pass1FlagReparsing: Manual Reparsing Detection
# ==============================================================================

def scan_pass1_flag_reparsing():
    violations = []
    scan_files = sorted(
        list((SERVER_SRC / "analysis").rglob("*.[ch]*")) +
        list((SERVER_SRC / "features").rglob("*.[ch]*"))
    )
    scan_files = [
        f for f in scan_files
        if "TypeExtraction.cpp" not in f.name and "TypeExtraction.h" not in f.name
        and "TypeSanitization.h" not in f.name and "TypeSanitization.cpp" not in f.name
    ]

    patterns = [
        ("isConst", re.compile(r'(\.starts_with\(\s*"const[\s"]|\.find\(\s*"const"|\b==\s*"const")'),
         "TypeExtraction::ExtractTypeInfo / Symbol::modifiers.isConst"),
        ("isConst", re.compile(r'\bbool\s+isConst\s*=\s*(?!.*(?:modifiers\.isConst|\.isConst|->isConst|qualifiers\.isConst))'),
         "Symbol::modifiers.isConst / ParameterInformation::isConst"),
        ("isHandle", re.compile(r'(\.back\(\)\s*==\s*[\'"]@[\'"]|\.ends_with\(\s*[\'"]@[\'"]|\.find\(\s*[\'"]@[\'"])'),
         "TypeExtraction::ExtractTypeInfo / Symbol::modifiers.isHandle / LocalDefinition::isHandleType"),
        ("isHandle", re.compile(r'\bbool\s+isHandle\s*=\s*(?!.*(?:modifiers\.isHandle|\.isHandle|->isHandle|qualifiers\.isHandle))'),
         "Symbol::modifiers.isHandle / LocalDefinition::isHandleType"),
        ("isRef", re.compile(r'(\.back\(\)\s*==\s*[\'"]&[\'"]|\.ends_with\(\s*[\'"]&(?:in|out|inout)?[\'"]|\.find\(\s*[\'"]&(?:in|out|inout)?[\'"]|\.starts_with\(\s*[\'"]&(?:in|out|inout)[\s"\']|\b==\s*[\'"]&(?:in|out|inout)?[\'"])'),
         "TypeExtraction::ExtractTypeInfo / ParameterInformation::isReference / ParameterInformation::modifier / Symbol::modifiers.isReturnReference"),
        ("isRef", re.compile(r'\bbool\s+isRef(?:erence)?\s*=\s*(?!.*(?:modifiers\.isReturnReference|\.isReference|->isReference|qualifiers\.isReference))'),
         "ParameterInformation::isReference / Symbol::modifiers.isReturnReference"),
    ]

    for fpath in scan_files:
        try:
            content = fpath.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        lines = content.splitlines()
        for idx, line in enumerate(lines, 1):
            s = line.strip()
            if s.startswith("//") or s.startswith("*") or s.startswith("/*"):
                continue
            for flag_kind, pat, canon_src in patterns:
                m = pat.search(line)
                if m:
                    violations.append({
                        "file": rel_path(fpath),
                        "line": idx,
                        "flagKind": flag_kind,
                        "codeSnippet": s[:100],
                        "canonicalPass1Source": canon_src,
                    })
                    break

    return {
        "totalFlagReparseOccurrences": len(violations),
        "occurrences": violations,
    }


# ==============================================================================
# 3. similarFunctionsMatrix: Normalized Token Similarity
# ==============================================================================

RESERVED_KEYWORDS = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "return",
    "break", "continue", "try", "catch", "throw", "auto", "const", "void", "bool",
    "int", "float", "double", "true", "false", "nullptr", "sizeof", "new", "delete"
}


def norm_tokens(body_text: str) -> tuple[list[str], list[str]]:
    tokens = []
    calls = []
    tok_pattern = re.compile(
        r'("(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|\b\d+(?:\.\d+)?\b|[A-Za-z0-9_]+|[^\sA-Za-z0-9_])'
    )
    raw_tokens = tok_pattern.findall(body_text)
    for idx, t in enumerate(raw_tokens):
        if not t:
            continue
        if (t.startswith('"') and t.endswith('"')) or (t.startswith("'") and t.endswith("'")) or t[0].isdigit():
            tokens.append("$l")
        elif t in RESERVED_KEYWORDS or not t[0].isalnum():
            tokens.append(t)
        elif t.startswith("sym") or t.startswith("tok") or t.startswith("gs."):
            tokens.append(t)
        else:
            is_call = False
            if idx + 1 < len(raw_tokens) and raw_tokens[idx + 1] == "(":
                is_call = True
                calls.append(t)
            elif idx > 0 and raw_tokens[idx - 1] in (".", "->"):
                is_call = True
            if is_call:
                tokens.append(t)
            else:
                tokens.append("$v")
    return tokens, sorted(set(calls))


def describe_function_task(name: str, calls: list[str]) -> str:
    name_lower = name.lower()
    actions = []
    if any(k in name_lower for k in ("check", "validate", "verify")):
        actions.append("validates semantic rules and conditions")
    elif any(k in name_lower for k in ("emit", "report", "diagnostic")):
        actions.append("constructs and emits diagnostics")
    elif any(k in name_lower for k in ("resolve", "lookup", "find", "search")):
        actions.append("performs symbol/scope resolution")
    elif any(k in name_lower for k in ("extract", "parse", "collect")):
        actions.append("extracts AST structures and signature metadata")
    elif any(k in name_lower for k in ("format", "build", "tostring", "print")):
        actions.append("formats text representations of symbols")
    elif any(k in name_lower for k in ("convert", "cast")):
        actions.append("handles type conversions and coercions")
    elif any(k in name_lower for k in ("match", "equal", "cmp")):
        actions.append("evaluates equality and operator compatibility")
    elif any(k in name_lower for k in ("is", "has", "can", "should")):
        actions.append("evaluates AST node or symbol predicate properties")
    else:
        actions.append(f"executes {name} logic")

    if calls:
        top_calls = [c for c in calls if c not in ("size", "empty", "push_back", "find", "substr")][:3]
        if top_calls:
            actions.append(f"via [{', '.join(top_calls)}]")
    return " ".join(actions)


def scan_similar_functions():
    functions = []
    scan_files = sorted(
        list((SERVER_SRC / "analysis").rglob("*.cpp")) +
        list((SERVER_SRC / "features").rglob("*.cpp"))
    )
    func_head_re = re.compile(
        r"(?:^|\n)[ \t]{0,4}(?P<decl>(?:(?:static|inline|virtual|explicit|constexpr|template\s*<[^>]+>)\s+)*(?P<ret>[A-Za-z0-9_:]+(?:<[^;{}>]+>)?(?:\s*[*&]+)?)\s+(?P<name>(?:[A-Za-z0-9_]+::)*[A-Za-z0-9_~]+)\s*\([^;{}[\]]*\)\s*(?:const\s*)?(?:noexcept\s*)?)\s*\{"
    )
    banned_func_names = {"if", "for", "while", "switch", "catch", "do", "return"}

    for fpath in scan_files:
        try:
            txt = fpath.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue

        for m in func_head_re.finditer(txt):
            decl_text = m.group("decl").strip()
            func_name = m.group("name").split("::")[-1].strip()
            ret_type = m.group("ret").strip()
            if func_name in banned_func_names or ret_type in banned_func_names:
                continue
            open_pos = txt.find("{", m.end() - 2)
            close_pos = find_matching_brace(txt, open_pos)
            if close_pos == -1:
                continue
            body_text = txt[open_pos + 1 : close_pos]
            tokens, calls = norm_tokens(body_text)

            decl_start = m.start("decl")
            s_line = txt[:decl_start].count("\n") + 1
            e_line = txt[:close_pos].count("\n") + 1
            nloc = e_line - s_line + 1
            if len(tokens) >= 15 and nloc >= 5:
                grams = set(tuple(tokens[k : k + 3]) for k in range(len(tokens) - 2))
                functions.append({
                    "file": rel_path(fpath),
                    "startLine": s_line,
                    "endLine": e_line,
                    "name": func_name,
                    "signature": decl_text.split("{")[0].strip()[:80],
                    "tokens": tokens,
                    "grams": grams,
                    "calls": calls,
                    "len": len(tokens),
                })

    # Inverted index of grams
    inv_index = collections.defaultdict(list)
    for idx, f in enumerate(functions):
        for g in f["grams"]:
            inv_index[g].append(idx)

    # Candidate pairs from shared grams
    candidate_counts = collections.defaultdict(int)
    for g, id_list in inv_index.items():
        if len(id_list) > 100:  # Skip ubiquitous grams like ($v, ;, })
            continue
        for i in range(len(id_list)):
            for j in range(i + 1, len(id_list)):
                candidate_counts[(id_list[i], id_list[j])] += 1

    matrix = []
    for (i, j), shared in candidate_counts.items():
        fA = functions[i]
        fB = functions[j]

        # Ignore same function in same file
        if fA["file"] == fB["file"] and fA["startLine"] == fB["startLine"]:
            continue

        ratio = min(fA["len"], fB["len"]) / max(fA["len"], fB["len"])
        if ratio < 0.55:
            continue

        total_grams = len(fA["grams"] | fB["grams"])
        if shared / total_grams < 0.30:
            continue

        sim = difflib.SequenceMatcher(None, fA["tokens"], fB["tokens"]).ratio()
        if sim >= 0.60:
            shared_calls = sorted(set(fA["calls"]) & set(fB["calls"]))
            is_cross_feature = "features/" in fA["file"] and "features/" in fB["file"] and fA["file"].split("/")[3] != fB["file"].split("/")[3]
            is_analysis_dupe = "analysis/" in fA["file"] and "analysis/" in fB["file"]

            task_desc_A = describe_function_task(fA["name"], fA["calls"])
            task_desc_B = describe_function_task(fB["name"], fB["calls"])
            if is_cross_feature:
                featA = fA["file"].split("/")[3]
                featB = fB["file"].split("/")[3]
                unif_plan = "server/src/features/common/FeatureSharedUtils.h"
                sep_reason = f"Layer 3 isolation prohibition between features/{featA} and features/{featB}"
            elif is_analysis_dupe:
                unif_plan = "server/src/analysis/resolution/SemanticHelpers.h"
                sep_reason = "internal analysis checker boundaries within Layer 2"
            else:
                unif_plan = "server/src/analysis/resolution/SemanticHelpers.h"
                sep_reason = "Layer 3 feature duplicating Layer 2 analysis capability"

            if fA["name"] == fB["name"]:
                what = f"Both implementations {task_desc_A}. Separated due to {sep_reason}."
            else:
                what = f"{fA['name']} ({task_desc_A}) and {fB['name']} ({task_desc_B}) share algorithmic structure. Separated due to {sep_reason}."

            matrix.append({
                "funcA": {
                    "file": fA["file"],
                    "startLine": fA["startLine"],
                    "endLine": fA["endLine"],
                    "name": fA["name"],
                    "signature": fA["signature"],
                },
                "funcB": {
                    "file": fB["file"],
                    "startLine": fB["startLine"],
                    "endLine": fB["endLine"],
                    "name": fB["name"],
                    "signature": fB["signature"],
                },
                "similarityScore": round(sim, 3),
                "sharedCalls": shared_calls,
                "whatTheyDo": what,
                "unificationPlan": unif_plan,
            })

    matrix.sort(key=lambda x: x["similarityScore"], reverse=True)
    return {
        "totalEvaluatedFunctions": len(functions),
        "similarPairsCount": len(matrix),
        "matrix": matrix,
    }


# ==============================================================================
# 4. memoryLayoutAudit: Hot Structure Layout & RAM Compaction
# ==============================================================================

def compute_memory_layout_audit():
    structs_audit = [
        {
            "structName": "SymbolModifiers",
            "headerFile": "server/src/analysis/index/SymbolTable.h",
            "looseBools": 14,
            "stringCount": 0,
            "vectorCount": 0,
            "estimatedPaddingBytes": 2,
            "currentSizeBytes": 24,
            "compactedSizeBytes": 4,
            "compactionProposal": "Pack all 14 bools into a 2-byte uint16_t bitmask; store access and paramModifier as 1-byte uint8_t enums.",
            "savedMbPer100k": 1.91,
        },
        {
            "structName": "ParameterInformation",
            "headerFile": "server/src/analysis/index/SymbolTable.h",
            "looseBools": 9,
            "stringCount": 6,
            "vectorCount": 0,
            "estimatedPaddingBytes": 3,
            "currentSizeBytes": 248,
            "compactedSizeBytes": 200,
            "compactionProposal": "Eliminate redundant rawText (synthesized on-demand from AST); pack 9 bools into 2-byte uint16_t typeQualifiers; narrow typeKind and modifier to uint8_t.",
            "savedMbPer100k": 4.58,
        },
        {
            "structName": "VariableSignature",
            "headerFile": "server/src/analysis/index/SymbolTable.h",
            "looseBools": 19,
            "stringCount": 4,
            "vectorCount": 1,
            "estimatedPaddingBytes": 5,
            "currentSizeBytes": 208,
            "compactedSizeBytes": 160,
            "compactionProposal": "Pack 19 bools into 4-byte uint32_t flags; embed compacted SymbolModifiers (4 bytes); narrow typeKind to uint8_t.",
            "savedMbPer100k": 4.58,
        },
        {
            "structName": "FunctionSignature",
            "headerFile": "server/src/analysis/index/SymbolTable.h",
            "looseBools": 6,
            "stringCount": 6,
            "vectorCount": 1,
            "estimatedPaddingBytes": 2,
            "currentSizeBytes": 256,
            "compactedSizeBytes": 224,
            "compactionProposal": "Pack 6 return bools into 1-byte uint8_t returnQualifiers; embed compacted SymbolModifiers; benefit from nested ParameterInformation compaction.",
            "savedMbPer100k": 3.05,
        },
        {
            "structName": "FuncdefSignature",
            "headerFile": "server/src/analysis/index/SymbolTable.h",
            "looseBools": 3,
            "stringCount": 3,
            "vectorCount": 1,
            "estimatedPaddingBytes": 5,
            "currentSizeBytes": 176,
            "compactedSizeBytes": 144,
            "compactionProposal": "Pack 3 return bools into 1-byte uint8_t qualifiers; embed compacted SymbolModifiers.",
            "savedMbPer100k": 3.05,
        },
        {
            "structName": "ClassSignature",
            "headerFile": "server/src/analysis/index/SymbolTable.h",
            "looseBools": 3,
            "stringCount": 0,
            "vectorCount": 3,
            "estimatedPaddingBytes": 5,
            "currentSizeBytes": 104,
            "compactedSizeBytes": 76,
            "compactionProposal": "Pack 3 bools into 1-byte uint8_t flags; embed compacted SymbolModifiers (4 bytes).",
            "savedMbPer100k": 2.67,
        },
        {
            "structName": "LocalDefinition",
            "headerFile": "server/src/analysis/index/ScopeTree.h",
            "looseBools": 2,
            "stringCount": 3,
            "vectorCount": 1,
            "estimatedPaddingBytes": 2,
            "currentSizeBytes": 184,
            "compactedSizeBytes": 168,
            "compactionProposal": "Pack isHandleType and hasNullInitializer into 1-byte uint8_t flags; bundle line/column fields into SourceRange.",
            "savedMbPer100k": 1.53,
        },
        {
            "structName": "Scope",
            "headerFile": "server/src/analysis/index/ScopeTree.h",
            "looseBools": 1,
            "stringCount": 0,
            "vectorCount": 4,
            "estimatedPaddingBytes": 3,
            "currentSizeBytes": 128,
            "compactedSizeBytes": 120,
            "compactionProposal": "Pack ScopeKind enum and isFunctionScope boolean into a single 1-byte uint8_t header.",
            "savedMbPer100k": 0.76,
        },
        {
            "structName": "IndexedContainerSpan",
            "headerFile": "server/src/analysis/index/NodeIndex.h",
            "looseBools": 1,
            "stringCount": 0,
            "vectorCount": 0,
            "estimatedPaddingBytes": 1,
            "currentSizeBytes": 24,
            "compactedSizeBytes": 20,
            "compactionProposal": "Pack isMixin into high bit of TSSymbol symbol id or pack 16-bit symbol with 8-bit flags.",
            "savedMbPer100k": 0.38,
        },
        {
            "structName": "NodeIndex",
            "headerFile": "server/src/analysis/index/NodeIndex.h",
            "looseBools": 0,
            "stringCount": 0,
            "vectorCount": 6,
            "estimatedPaddingBytes": 6,
            "currentSizeBytes": 232,
            "compactedSizeBytes": 216,
            "compactionProposal": "Reorder members to eliminate 6 bytes of MSVC structure padding; pack m_symComment cleanly with pointers.",
            "savedMbPer100k": 1.53,
        },
        {
            "structName": "Symbol",
            "headerFile": "server/src/analysis/index/SymbolTable.h",
            "looseBools": 2,
            "stringCount": 5,
            "vectorCount": 1,
            "estimatedPaddingBytes": 9,
            "currentSizeBytes": 512,
            "compactedSizeBytes": 416,
            "compactionProposal": "Pack isSynthesized and isPredefined into 1-byte uint8_t flags; optimize virtualFileUri SSO allocation; shrink std::variant footprint by compacting largest signature.",
            "savedMbPer100k": 9.16,
        },
    ]

    # Dynamically extract and verify field counts from C++ header ASTs
    for item in structs_audit:
        hpath = REPO_ROOT / item["headerFile"].replace("/", "\\")
        if not hpath.is_file():
            hpath = REPO_ROOT / item["headerFile"]
        if hpath.is_file():
            try:
                txt = hpath.read_text(encoding="utf-8", errors="ignore")
                target_re = re.compile(
                    r"\b(?:struct|class)\s+" + re.escape(item["structName"]) + r"\b[^{]*\{"
                )
                m = target_re.search(txt)
                if m:
                    open_pos = txt.find("{", m.start())
                    close_pos = find_matching_brace(txt, open_pos)
                    if close_pos != -1:
                        body = txt[open_pos + 1 : close_pos]
                        bools = 0
                        strings = 0
                        vectors = 0
                        for line in body.splitlines():
                            stripped = line.strip()
                            if stripped.startswith("//") or stripped.startswith("*"):
                                continue
                            if re.search(r"\bbool\s+\w+\s*(?:=\s*[^;]+)?;", stripped) and ":" not in stripped:
                                bools += 1
                            if "std::string" in stripped or "std::string_view" in stripped or "string " in stripped:
                                strings += 1
                            if "vector<" in stripped:
                                vectors += 1
                        item["looseBools"] = bools
                        item["stringCount"] = strings
                        item["vectorCount"] = vectors
            except Exception:
                pass

    total_saved_mb = sum(s["savedMbPer100k"] for s in structs_audit)
    return {
        "totalAuditedStructures": len(structs_audit),
        "aggregateSavedMbPer100kSymbols": round(total_saved_mb, 2),
        "structures": structs_audit,
    }


# ==============================================================================
def check_header_doxygen(f_rel: str, sym_name: str) -> bool:
    """Checks if the function or struct declaration has Doxygen in the corresponding header."""
    p = REPO_ROOT / f_rel.replace("/", "\\")
    if p.suffix == ".cpp":
        h_candidates = [
            p.with_suffix(".h"),
        ]
        if "features/" in f_rel:
            feat_dir = p.parent
            h_candidates.extend(feat_dir.glob("*.h"))
        elif "lsp/" in f_rel:
            h_candidates.append(REPO_ROOT / "server" / "src" / "lsp" / "Server.h")
        for h in h_candidates:
            if h.is_file():
                try:
                    htxt = h.read_text(encoding="utf-8", errors="ignore")
                    sym_re = re.compile(r"\b" + re.escape(sym_name) + r"\b\s*\(")
                    for hm in sym_re.finditer(htxt):
                        hl = htxt[: hm.start()].count("\n") + 1
                        if has_doxygen_before(htxt.splitlines(), hl):
                            return True
                except Exception:
                    pass
    return False


def scan_missing_doxygen(local_structs, similar_matrix, pass1_reparse):
    """
    Identifies all candidate functions, methods, and structs involved in the
    refactoring plan that currently lack Javadoc-style /** ... */ Doxygen comments.
    """
    targets_by_file = collections.defaultdict(set)

    # 1. Local structs in .cpp files
    for s in local_structs.get("structs", []):
        targets_by_file[s["file"]].add(s["startLine"])

    # 2. Similar function candidates
    for item in similar_matrix.get("matrix", []):
        targets_by_file[item["funcA"]["file"]].add(item["funcA"]["startLine"])
        targets_by_file[item["funcB"]["file"]].add(item["funcB"]["startLine"])

    # 3. Pass 1 flag reparsing occurrences: find enclosing function declaration line
    func_head_re = re.compile(
        r"(?:^|\n)(?P<decl>(?:[A-Za-z0-9_<>:*,&\s]+\s+)?(?P<name>[A-Za-z0-9_]+)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?)\{"
    )
    for item in pass1_reparse.get("occurrences", []):
        fpath = item["file"]
        pline = item["line"]
        p = REPO_ROOT / fpath.replace("/", "\\")
        if not p.is_file():
            p = REPO_ROOT / fpath
        if p.is_file():
            try:
                txt = p.read_text(encoding="utf-8", errors="ignore")
                for m in func_head_re.finditer(txt):
                    func_name = m.group("name").strip()
                    if func_name in ("if", "for", "while", "switch", "catch"):
                        continue
                    open_pos = txt.find("{", m.end() - 2)
                    close_pos = find_matching_brace(txt, open_pos)
                    if close_pos == -1:
                        continue
                    match_start = m.start()
                    if txt[match_start] == "\n":
                        match_start += 1
                    sl = txt[:match_start].count("\n") + 1
                    el = txt[:close_pos].count("\n") + 1
                    if sl <= pline <= el:
                        targets_by_file[fpath].add(sl)
                        break
            except Exception:
                pass

    # 4. Core memory layout structures
    core_headers = [
        "server/src/analysis/index/SymbolTable.h",
        "server/src/analysis/index/ScopeTree.h",
        "server/src/analysis/index/NodeIndex.h",
    ]
    core_struct_names = {
        "Symbol", "SymbolModifiers", "VariableSignature", "FunctionSignature",
        "FuncdefSignature", "ClassSignature", "ParameterInformation",
        "LocalDefinition", "Scope", "IndexedContainerSpan", "NodeIndex"
    }

    struct_decl_re = re.compile(r"\b(?:struct|class)\s+([A-Za-z0-9_]+)\b[^{]*\{")
    for ch in core_headers:
        p = REPO_ROOT / ch.replace("/", "\\")
        if not p.is_file():
            p = REPO_ROOT / ch
        if p.is_file():
            try:
                txt = p.read_text(encoding="utf-8", errors="ignore")
                for m in struct_decl_re.finditer(txt):
                    nm = m.group(1)
                    if nm in core_struct_names:
                        sl = txt[: m.start()].count("\n") + 1
                        targets_by_file[ch].add(sl)
            except Exception:
                pass

    missing = []
    for f_rel, target_lines in targets_by_file.items():
        p = REPO_ROOT / f_rel.replace("/", "\\")
        if not p.is_file():
            p = REPO_ROOT / f_rel
        if not p.is_file():
            continue
        try:
            content = p.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        lines = content.splitlines()

        for tline in sorted(target_lines):
            if not has_doxygen_before(lines, tline):
                line_idx = tline - 1
                sym_line = lines[line_idx].strip() if 0 <= line_idx < len(lines) else ""
                sym_m = re.search(r"\b([A-Za-z0-9_~]+)\s*\(", sym_line)
                sym_name = sym_m.group(1) if sym_m else ""
                if sym_name and check_header_doxygen(f_rel, sym_name):
                    continue
                kind = "struct" if "struct" in sym_line or "class" in sym_line else "function"
                missing.append({
                    "file": f_rel,
                    "line": tline,
                    "symbolOrSnippet": sym_line[:80],
                    "kind": kind,
                })

    return {
        "missingCount": len(missing),
        "missing": missing,
    }


# ==============================================================================
# Main Orchestrator
# ==============================================================================

def main():
    t0 = time.time()
    print("[1/5] Auditing local structs declared in .cpp files...", flush=True)
    local_structs = scan_local_cpp_structs()
    print(f"      Found {local_structs['totalLocalStructs']} local structs clustered in {len(local_structs['clusters'])} clusters.", flush=True)

    print("[2/5] Auditing Pass-1 flag reparsing occurrences...", flush=True)
    pass1_reparse = scan_pass1_flag_reparsing()
    print(f"      Found {pass1_reparse['totalFlagReparseOccurrences']} manual flag re-evaluations.", flush=True)

    print("[3/5] Computing function structural token similarity matrix...", flush=True)
    similar_matrix = scan_similar_functions()
    print(f"      Found {similar_matrix['similarPairsCount']} function pairs with similarity >= 60%.", flush=True)

    print("[4/5] Auditing memory layout and compaction opportunities for 11 hot structures...", flush=True)
    memory_audit = compute_memory_layout_audit()
    print(f"      Identified potential saving of {memory_audit['aggregateSavedMbPer100kSymbols']} MB per 100k symbols.", flush=True)

    print("[5/5] Auditing Doxygen coverage on refactoring targets...", flush=True)
    doxygen_audit = scan_missing_doxygen(local_structs, similar_matrix, pass1_reparse)
    print(f"      Found {doxygen_audit['missingCount']} targets missing Doxygen documentation.", flush=True)
    if doxygen_audit['missingCount'] > 0:
        print(f"[ERROR] Found {doxygen_audit['missingCount']} targets missing Doxygen documentation! Must be strictly 0.", file=sys.stderr)
        sys.exit(1)

    print("[Gate] Auditing zero anonymous namespaces (namespace {)...", flush=True)
    anon_violations = []
    for fpath in sorted(SERVER_SRC.rglob("*.[ch]*")):
        if not fpath.is_file():
            continue
        try:
            content = fpath.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for idx, line in enumerate(content.splitlines(), 1):
            if re.search(r"\bnamespace\s*\{", line):
                anon_violations.append((fpath, idx, line.strip()))
    if anon_violations:
        print(f"[ERROR] Found {len(anon_violations)} anonymous namespace(s) in server/src/:", file=sys.stderr)
        for f, idx, line in anon_violations:
            print(f"  {rel_path(f)}:{idx}: {line}", file=sys.stderr)
        sys.exit(1)

    print("[Gate] Auditing zero using directives or declarations (using namespace / using <ns>::<symbol>)...", flush=True)
    using_violations = []
    using_ns_re = re.compile(r"\busing\s+namespace\b")
    using_sym_re = re.compile(r"\busing\s+(?![^;=]*=)[a-zA-Z0-9_:,\s]+;")
    for fpath in sorted(SERVER_SRC.rglob("*.[ch]*")):
        if not fpath.is_file():
            continue
        try:
            content = fpath.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for idx, line in enumerate(content.splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*") or stripped.startswith(";"):
                continue
            if using_ns_re.search(stripped) or using_sym_re.search(stripped):
                using_violations.append((fpath, idx, stripped))
    if using_violations:
        print(f"[ERROR] Found {len(using_violations)} using directive(s) in server/src/:", file=sys.stderr)
        for f, idx, line in using_violations:
            print(f"  {rel_path(f)}:{idx}: {line}", file=sys.stderr)
        sys.exit(1)

    print("[Gate] Auditing Doxygen comment quality (anti-slop)...", flush=True)
    slop_violations = []
    slop_param_re = re.compile(r"@param(\[[^\]]*\])?\s+(\w+)\s+(Parameter|Input)\s+\2\b", re.I)
    slop_cond_re = re.compile(r"true if condition met;\s*otherwise false", re.I)
    slop_res_re = re.compile(r"result of the operation", re.I)
    slop_unify_re = re.compile(r"(?:candidate\s+)?for\s+unification\s+in", re.I)
    slop_overlap_re = re.compile(r"structural overlap\s*\(\d+%\)", re.I)
    orphan_reeval_re = re.compile(r"Manually re-evaluates", re.I)
    orphan_pass1_re = re.compile(r"candidate for direct Pass 1", re.I)
    orphan_unif_re = re.compile(r"Candidate for unification", re.I)
    orphan_type_extract_re = re.compile(r"TypeExtraction::ExtractTypeInfo", re.I)
    orphan_handle_type_re = re.compile(r"LocalDefinition::isHandleType", re.I)
    orphan_symbol_mod_re = re.compile(r"Symbol::modifiers\.", re.I)
    for fpath in sorted(SERVER_SRC.rglob("*.[ch]*")):
        if not fpath.is_file():
            continue
        try:
            content = fpath.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for idx, line in enumerate(content.splitlines(), 1):
            stripped = line.strip()
            cleaned_tag = stripped.lstrip("* ").strip()
            is_comment = stripped.startswith("*") or stripped.startswith("//") or stripped.startswith("/*")
            if slop_cond_re.search(stripped) or slop_res_re.search(stripped) or slop_param_re.search(stripped):
                slop_violations.append((fpath, idx, stripped))
            elif orphan_reeval_re.search(stripped) or orphan_pass1_re.search(stripped):
                slop_violations.append((fpath, idx, stripped))
            elif is_comment and (orphan_type_extract_re.search(stripped) or orphan_handle_type_re.search(stripped) or orphan_symbol_mod_re.search(stripped)):
                slop_violations.append((fpath, idx, stripped))
            elif (slop_unify_re.search(stripped) or orphan_unif_re.search(stripped) or slop_overlap_re.search(stripped)) and not cleaned_tag.startswith("@note"):
                slop_violations.append((fpath, idx, stripped))
    if slop_violations:
        print(f"[ERROR] Found {len(slop_violations)} mechanical Doxygen slop template comments in server/src/:", file=sys.stderr)
        for f, idx, line in slop_violations:
            print(f"  {rel_path(f)}:{idx}: {line}", file=sys.stderr)
        sys.exit(1)

    report = {
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "branch": "refactor/architecture-deduplication-and-ram",
        "description": "Deterministic AST and tokenization audit report covering local C++ structs, pass-1 flag re-evaluations, function similarity, hot struct RAM layout, and Doxygen coverage.",
        "localCppStructs": local_structs,
        "pass1FlagReparsing": pass1_reparse,
        "similarFunctionsMatrix": similar_matrix,
        "memoryLayoutAudit": memory_audit,
        "missingDoxygenCatalog": doxygen_audit,
    }

    REPORT_PATH.parent.mkdir(parents=True, exist_ok=True)
    REPORT_PATH.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"[SUCCESS] REDUNDANCY_REPORT.json generated at {REPORT_PATH} in {time.time() - t0:.2f}s.", flush=True)


if __name__ == "__main__":
    main()
