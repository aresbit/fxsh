#!/usr/bin/env python3
"""
Generate fxsh FFI bindings from C headers via clang AST JSON.

MVP scope:
- function declarations -> `let name : ... = "c:...:symbol"`
- enum constants -> `let name : int = c_const_int "ENUM_NAME"`
- struct/union typedef forward decls -> `let _ : unit = cdef "...;"`
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Set, Tuple


@dataclass(frozen=True)
class FuncDecl:
    name: str
    ret_ty: str
    arg_tys: Tuple[str, ...]
    variadic: bool
    source_file: str


@dataclass(frozen=True)
class EnumConst:
    name: str
    source_file: str


def run_clang_ast(header: str, clang_args: List[str]) -> Dict:
    if os.path.exists(header):
        include_line = f'#include "{header}"\n'
    else:
        include_line = f"#include <{header}>\n"

    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        probe = f.name
        f.write(include_line)

    try:
        cmd = [
            "clang",
            "-x",
            "c",
            "-std=gnu17",
            "-Xclang",
            "-ast-dump=json",
            "-fsyntax-only",
            probe,
            *clang_args,
        ]
        out = subprocess.check_output(cmd, stderr=subprocess.PIPE)
        return json.loads(out.decode("utf-8"))
    except subprocess.CalledProcessError as e:
        sys.stderr.write("clang AST dump failed\n")
        sys.stderr.write(e.stderr.decode("utf-8", errors="ignore"))
        raise
    finally:
        try:
            os.unlink(probe)
        except OSError:
            pass


def node_file(node: Dict) -> str:
    for k in ("loc", "range"):
        obj = node.get(k, {})
        if k == "range":
            obj = obj.get("begin", {})
        f = obj.get("file")
        if isinstance(f, str):
            return f
    return ""


def iter_nodes(root: Dict) -> Iterable[Dict]:
    stack = [root]
    while stack:
        n = stack.pop()
        yield n
        inner = n.get("inner")
        if isinstance(inner, list):
            for c in reversed(inner):
                if isinstance(c, dict):
                    stack.append(c)


def norm_spaces(s: str) -> str:
    return re.sub(r"\s+", " ", s.strip())


def strip_qualifiers(ty: str) -> str:
    ty = norm_spaces(ty)
    ty = re.sub(r"\b(const|volatile|restrict)\b", "", ty)
    return norm_spaces(ty)


def ptr_level(ty: str) -> int:
    return ty.count("*")


def base_without_ptr(ty: str) -> str:
    return norm_spaces(ty.replace("*", ""))


def c_type_to_fxsh(ty: str, is_return: bool = False) -> str:
    raw = strip_qualifiers(ty)
    plev = ptr_level(raw)
    base = base_without_ptr(raw)

    if raw == "void":
        return "unit"

    if plev > 0:
        if plev == 1 and base in ("char", "signed char", "unsigned char"):
            return "string"
        return "unit ptr"

    if base in ("_Bool", "bool"):
        return "bool"
    if base in ("float", "double"):
        return "float"
    if base in ("int",):
        return "c_int"
    if base in ("unsigned int",):
        return "c_uint"
    if base in ("long", "long int"):
        return "c_long"
    if base in ("unsigned long", "unsigned long int"):
        return "c_ulong"
    if base in ("size_t",):
        return "c_size"
    if base in ("ssize_t",):
        return "c_ssize"
    if base.startswith("enum "):
        return "int"
    if base in (
        "short",
        "short int",
        "unsigned short",
        "unsigned short int",
        "long long",
        "long long int",
        "unsigned long long",
        "unsigned long long int",
        "char",
        "signed char",
        "unsigned char",
        "int8_t",
        "uint8_t",
        "int16_t",
        "uint16_t",
        "int32_t",
        "uint32_t",
        "int64_t",
        "uint64_t",
    ):
        return "int"
    return "unit ptr" if is_return else "int"


def sanitize_ident(name: str) -> str:
    out = re.sub(r"[^a-zA-Z0-9_]", "_", name)
    if not out:
        out = "v"
    if out[0].isdigit():
        out = "c_" + out
    return out


def snake_lower(name: str) -> str:
    s = re.sub(r"[^a-zA-Z0-9]+", "_", name).strip("_")
    s = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", s)
    s = s.lower()
    if not s:
        s = "c_const"
    if s[0].isdigit():
        s = "c_" + s
    return s


def function_fxsh_type(ret_ty: str, arg_tys: Iterable[str]) -> str:
    args = list(arg_tys)
    if not args:
        return f"unit -> {ret_ty}"
    return " -> ".join(args + [ret_ty])


def pick_source_match(path: str, header: str) -> bool:
    if not path:
        return True
    hb = os.path.basename(header)
    pb = os.path.basename(path)
    if hb == pb:
        return True
    # Header can be include form like sqlite3.h while path is full path.
    if path.endswith("/" + hb):
        return True
    # Some system-header nodes report only includedFrom temp probe file.
    # Keep them and rely on symbol-prefix filtering for precision.
    if pb.endswith(".h"):
        return True
    return False


def collect_decls(ast: Dict, header: str) -> Tuple[List[FuncDecl], List[EnumConst], Set[str], Set[str]]:
    funcs: Dict[str, FuncDecl] = {}
    enums: Dict[str, EnumConst] = {}
    struct_names: Set[str] = set()
    union_names: Set[str] = set()

    for n in iter_nodes(ast):
        kind = n.get("kind")
        src = node_file(n)

        if kind == "RecordDecl":
            if not pick_source_match(src, header):
                continue
            tag_used = n.get("tagUsed", "")
            name = n.get("name", "")
            if name:
                if tag_used == "struct":
                    struct_names.add(name)
                elif tag_used == "union":
                    union_names.add(name)

        if kind == "TypedefDecl":
            if not pick_source_match(src, header):
                continue
            t = n.get("type", {}).get("qualType", "")
            nm = n.get("name", "")
            m = re.fullmatch(r"(struct|union)\s+([A-Za-z_][A-Za-z0-9_]*)", strip_qualifiers(t))
            if m and nm:
                if m.group(1) == "struct":
                    struct_names.add(m.group(2))
                else:
                    union_names.add(m.group(2))

        if kind == "EnumConstantDecl":
            if pick_source_match(src, header):
                nm = n.get("name")
                if isinstance(nm, str) and nm:
                    enums[nm] = EnumConst(name=nm, source_file=src)

        if kind != "FunctionDecl":
            continue
        if not pick_source_match(src, header):
            continue
        name = n.get("name")
        if not isinstance(name, str) or not name:
            continue
        if name.startswith("__builtin_"):
            continue
        if n.get("storageClass") == "static":
            continue
        t = n.get("type", {}).get("qualType", "")
        if not isinstance(t, str) or "(" not in t:
            continue
        ret_raw = t.split("(", 1)[0].strip()
        ret_fx = c_type_to_fxsh(ret_raw, is_return=True)

        arg_tys: List[str] = []
        variadic = bool(n.get("isVariadic", False))
        for c in n.get("inner", []):
            if c.get("kind") != "ParmVarDecl":
                continue
            pty = c.get("type", {}).get("qualType", "")
            if not isinstance(pty, str):
                continue
            arg_tys.append(c_type_to_fxsh(pty, is_return=False))

        funcs[name] = FuncDecl(
            name=name,
            ret_ty=ret_fx,
            arg_tys=tuple(arg_tys),
            variadic=variadic,
            source_file=src,
        )

    return sorted(funcs.values(), key=lambda x: x.name), sorted(enums.values(), key=lambda x: x.name), struct_names, union_names


def render_fxsh(
    header: str,
    lib: str,
    funcs: List[FuncDecl],
    enums: List[EnumConst],
    struct_names: Set[str],
    union_names: Set[str],
) -> str:
    lines: List[str] = []
    lines.append("# Auto-generated by tools/fxsh_cimport.py")
    lines.append("# Requires native-codegen mode.")
    lines.append(f'let _ : unit = c_include "{header}"')
    lines.append("")

    for n in sorted(struct_names):
        lines.append(f'let _ : unit = cdef "typedef struct {n} {n};"')
    for n in sorted(union_names):
        lines.append(f'let _ : unit = cdef "typedef union {n} {n};"')
    if struct_names or union_names:
        lines.append("")

    if enums:
        lines.append("# enum/macro integer constants")
        used: Set[str] = set()
        for e in enums:
            ident = snake_lower(e.name)
            base = ident
            i = 2
            while ident in used:
                ident = f"{base}_{i}"
                i += 1
            used.add(ident)
            lines.append(f'let {ident} : int = c_const_int "{e.name}"')
        lines.append("")

    if funcs:
        lines.append("# function bindings")
        skipped = 0
        for f in funcs:
            if f.variadic:
                skipped += 1
                lines.append(f"# skipped variadic: {f.name}")
                continue
            fident = sanitize_ident(f.name)
            fty = function_fxsh_type(f.ret_ty, f.arg_tys)
            if lib:
                bind = f'c:{lib}:{f.name}'
            else:
                bind = f"c::{f.name}"
            lines.append(f'let {fident} : {fty} = "{bind}"')
        if skipped:
            lines.append("")
            lines.append(f"# skipped {skipped} variadic declarations")

    return "\n".join(lines) + "\n"


def main() -> int:
    p = argparse.ArgumentParser(description="Generate fxsh FFI bindings from C headers.")
    p.add_argument("--header", required=True, help='Header path or include name, e.g. "sqlite3.h".')
    p.add_argument("--lib", default="", help='Library path/name for dynamic FFI, e.g. "/usr/lib/libsqlite3.dylib".')
    p.add_argument("--out", required=True, help="Output fxsh file path.")
    p.add_argument(
        "--clang-arg",
        action="append",
        default=[],
        help="Extra clang args (repeatable), e.g. --clang-arg=-I/opt/include",
    )
    p.add_argument(
        "--symbol-prefix",
        default="",
        help='Only keep declarations whose C name starts with this prefix, e.g. "sqlite3_".',
    )
    p.add_argument(
        "--enum-prefix",
        action="append",
        default=[],
        help='Enum-constant prefix filter (repeatable), e.g. --enum-prefix=SQLITE_.',
    )
    args = p.parse_args()

    ast = run_clang_ast(args.header, args.clang_arg)
    funcs, enums, structs, unions = collect_decls(ast, args.header)
    if args.symbol_prefix:
        funcs = [f for f in funcs if f.name.startswith(args.symbol_prefix)]
        p = args.symbol_prefix
        structs = {s for s in structs if s.startswith(p) or s.startswith(p.rstrip("_"))}
        unions = {u for u in unions if u.startswith(p) or u.startswith(p.rstrip("_"))}
    if args.enum_prefix:
        enums = [e for e in enums if any(e.name.startswith(ep) for ep in args.enum_prefix)]
    elif args.symbol_prefix:
        enums = []
    out = render_fxsh(args.header, args.lib, funcs, enums, structs, unions)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(out)

    sys.stderr.write(
        f"generated {args.out}: funcs={len(funcs)} enums={len(enums)} structs={len(structs)} unions={len(unions)}\n"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
