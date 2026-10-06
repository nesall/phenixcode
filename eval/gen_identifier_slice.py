#!/usr/bin/env python3
"""
Generate an identifier-lookup eval slice for the phenixcode retrieval eval.

Scans src/*.cpp and include/*.h for function definitions and UPPER_SNAKE constants,
keeps only names with a single, unambiguous defining file, and writes a JSON array of
{"query", "expected_doc_ids"} entries (plus "slice" / "symbol" for debugging; the eval
loader ignores extra keys).

Usage:
  python gen_identifier_slice.py --root D:/workspace/projects/phenixcode --out eval_identifiers.json
  python gen_identifier_slice.py --root . --funcs 25 --consts 5 --seed 7 --with-header

--root must be spelled exactly like the source_id prefix stored in the DB
(forward slashes; e.g. D:/workspace/projects/phenixcode), because doc ids are compared
as plain strings. Check one "doc_id" in a previous eval output.
"""
import argparse
import json
import random
import re
import sys
from collections import defaultdict
from pathlib import Path

KEYWORDS = {
    "if", "for", "while", "switch", "return", "else", "do", "catch", "namespace", "class",
    "struct", "enum", "union", "typedef", "using", "template", "new", "delete", "sizeof",
    "case", "throw", "static_assert", "try", "defined",
}

FUNC_TEMPLATES = [
    "What does {n} function do?",
    "What does the {n} function do?",
    "Where is {n} defined?",
    "How does {n} work?",
    "Explain what {n} is for.",
    "What does `{q}` do?",
]
CONST_TEMPLATES = [
    "What is {n} used for?",
    "Where is {n} defined?",
]


def blank_out(text: str) -> str:
    """Replace comments and raw-string bodies with spaces, preserving newlines/offsets."""
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))

    text = re.sub(r'R"([^()\\\s]{0,16})\((.*?)\)\1"', blank, text, flags=re.S)
    text = re.sub(r"/\*.*?\*/", blank, text, flags=re.S)
    text = re.sub(r"//[^\n]*", blank, text)
    return text


def is_identifier_like(name: str) -> bool:
    """Same shape rule as isIdentifierLike() in database.cpp."""
    if "_" in name:
        return True
    has_digit = any(c.isdigit() for c in name)
    has_alpha = any(c.isalpha() for c in name)
    has_lower = any(c.islower() for c in name)
    inner_upper = any(c.isupper() for c in name[1:])
    return (has_lower and inner_upper) or (has_digit and has_alpha)


def find_definitions(text: str):
    """Yield (qualified_name, name) for column-0 function definitions."""
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        line = lines[i]
        if not line or line[0].isspace() or line[0] in "#}{)" or not (line[0].isalpha() or line[0] in "_~"):
            i += 1
            continue
        # join up to 8 lines until the signature ends in '{' or ';'
        sig = line
        j = i
        while j < min(i + 8, len(lines) - 1) and "{" not in sig and ";" not in sig:
            j += 1
            sig += " " + lines[j].strip()
        brace = sig.find("{")
        semi = sig.find(";")
        if brace < 0 or (0 <= semi < brace):
            i += 1
            continue
        head = sig[:brace]
        paren = head.find("(")
        if paren < 0:
            i += 1
            continue
        before = head[:paren].rstrip()
        m = re.search(r"((?:[A-Za-z_]\w*(?:<[^<>]*>)?::)*~?[A-Za-z_]\w*)$", before)
        if not m:
            i += 1
            continue
        qual = m.group(1)
        prefix = before[: m.start()].strip()
        parts = [re.sub(r"<[^<>]*>", "", p) for p in qual.split("::")]
        name = parts[-1]
        if (
            name in KEYWORDS
            or parts[0] in KEYWORDS
            or "=" in prefix
            or "operator" in name
            or name.startswith("~")
            or (len(parts) > 1 and parts[-1] == parts[-2])  # constructor
            or (not prefix and len(parts) == 1)             # macro / call, not a definition
        ):
            i += 1
            continue
        yield "::".join(parts), name
        i = j + 1 if j > i else i + 1


CONST_RES = [
    re.compile(r"^\s*#\s*define\s+([A-Z][A-Z0-9_]{3,})\b", re.M),
    re.compile(
        r"^\s*(?:(?:static|inline|extern)\s+)*(?:constexpr|const)\s+[\w:<>\s\*&]+?\b([A-Z][A-Z0-9_]{3,})\s*(?:=|\{|\[)",
        re.M,
    ),
]


def find_constants(text: str):
    for rx in CONST_RES:
        for m in rx.finditer(text):
            if "_" in m.group(1):
                yield m.group(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", required=True, help="project root (same prefix as DB source_id)")
    ap.add_argument("--out", default="eval_identifiers.json")
    ap.add_argument("--funcs", type=int, default=20, help="number of function queries")
    ap.add_argument("--consts", type=int, default=5, help="number of constant queries")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--min-len", type=int, default=6, help="minimum identifier length")
    ap.add_argument("--exclude", nargs="*", default=["tests.cpp"], help="file names to skip")
    ap.add_argument("--with-header", action="store_true",
                    help="also list a header that mentions the function as an expected doc")
    args = ap.parse_args()

    root = Path(args.root)
    prefix = args.root.replace("\\", "/").rstrip("/")
    files = sorted(list(root.glob("src/*.cpp")) + list(root.glob("include/*.h")))
    files = [f for f in files if f.name not in set(args.exclude)]
    if not files:
        sys.exit(f"No sources found under {root}/src and {root}/include")

    type_names = set()
    func_defs = defaultdict(set)    # unqualified name -> {(relpath, qualified)}
    const_defs = defaultdict(set)   # constant -> {relpath}
    raw_text = {}
    for f in files:
        rel = f.relative_to(root).as_posix()
        text = blank_out(f.read_text(encoding="utf-8", errors="replace"))
        raw_text[rel] = text
        type_names.update(re.findall(r"\b(?:class|struct|enum(?:\s+class)?)\s+([A-Za-z_]\w*)", text))
        for qual, name in find_definitions(text):
            func_defs[name].add((rel, qual))
        for c in find_constants(text):
            const_defs[c].add(rel)

    rng = random.Random(args.seed)

    # --- functions: unique definition, identifier-shaped, not a trivial name
    func_cands = defaultdict(list)  # file -> [(name, qual)]
    for name, defs in func_defs.items():
        if (len(defs) != 1 or len(name) < args.min_len or not is_identifier_like(name)
                or name in type_names or name[0].isupper()):
            continue
        rel, qual = next(iter(defs))
        func_cands[rel].append((name, qual))

    const_cands = defaultdict(list)
    for c, rels in const_defs.items():
        if len(rels) == 1 and len(c) >= args.min_len:
            const_cands[next(iter(rels))].append(c)

    def round_robin(cands, n):
        buckets = {k: rng.sample(v, len(v)) for k, v in sorted(cands.items())}
        picked = []
        while len(picked) < n and any(buckets.values()):
            for k in sorted(buckets):
                if buckets[k] and len(picked) < n:
                    picked.append((k, buckets[k].pop()))
        return picked

    entries = []
    for rel, (name, qual) in round_robin(func_cands, args.funcs):
        tpl = rng.choice(FUNC_TEMPLATES)
        query = tpl.format(n=name, q=qual)
        expected = [f"{prefix}/{rel}"]
        if args.with_header:
            call = re.compile(rf"\b{re.escape(name)}\s*\(")
            for hrel, htext in raw_text.items():
                if hrel.endswith(".h") and hrel != rel and call.search(htext):
                    expected.append(f"{prefix}/{hrel}")
        entries.append({"query": query, "expected_doc_ids": expected,
                        "slice": "identifier_function", "symbol": qual})

    for rel, c in round_robin(const_cands, args.consts):
        entries.append({"query": rng.choice(CONST_TEMPLATES).format(n=c),
                        "expected_doc_ids": [f"{prefix}/{rel}"],
                        "slice": "identifier_constant", "symbol": c})

    Path(args.out).write_text(json.dumps(entries, indent=2), encoding="utf-8")

    print(f"scanned {len(files)} files; "
          f"{sum(1 for d in func_defs.values() if len(d) == 1)} uniquely-defined functions, "
          f"{sum(len(v) for v in func_cands.values())} usable; "
          f"{sum(len(v) for v in const_cands.values())} usable constants", file=sys.stderr)
    print(f"wrote {len(entries)} entries -> {args.out}", file=sys.stderr)
    for e in entries[:8]:
        print(f"  {e['query']!r:70} -> {e['expected_doc_ids'][0].rsplit('/', 1)[-1]}", file=sys.stderr)


if __name__ == "__main__":
    main()