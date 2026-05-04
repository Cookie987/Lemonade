#!/usr/bin/env python3
import argparse
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC_TXT = ROOT / "lvgl_bindings.txt"
SPEC_JSON = ROOT / "lvgl_bindings.json"
OUT = ROOT / "lua_lvgl_gen.h"
HEADER_SKIP_PARTS = {"docs", "tests", "demos", "examples", "env_support", "__pycache__"}
CONSTANT_HEADER_SKIP_PARTS = HEADER_SKIP_PARTS | {"debugging", "drivers", "libs", "osal"}

INT_TYPES = {
    "int",
    "unsigned",
    "unsigned int",
    "short",
    "unsigned short",
    "long",
    "unsigned long",
    "int8_t",
    "uint8_t",
    "int16_t",
    "uint16_t",
    "int32_t",
    "uint32_t",
    "int64_t",
    "uint64_t",
    "size_t",
    "lv_coord_t",
}

OBJ_PTR_TYPES = {"lv_obj_t *", "const lv_obj_t *"}
STRING_TYPES = {"char *", "const char *"}
VOID_PTR_TYPES = {"void *", "const void *"}

MANUAL_BINDINGS = {
    "lv_label_set_text_fmt",
    "lv_dropdown_get_selected_str",
    "lv_img_set_src",
    "lv_obj_set_style_pad_all",
    "lv_obj_set_style_pad_hor",
    "lv_obj_set_style_pad_ver",
    "lv_obj_set_style_pad_gap",
    "lv_obj_set_style_size",
    "lv_timer_create",
    "lv_timer_del",
}


def parse_alias_macros(text: str) -> dict[str, str]:
    aliases: dict[str, str] = {}
    for line in text.splitlines():
        m = re.match(r"^\s*#define\s+(lv_[A-Za-z0-9_]+)\s+(lv_[A-Za-z0-9_]+)\b", line)
        if not m:
            continue
        aliases[m.group(1)] = m.group(2)
    return aliases


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//.*?$", " ", text, flags=re.M)
    text = re.sub(r"^\s*#.*?$", " ", text, flags=re.M)
    return text


def strip_c_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//.*?$", " ", text, flags=re.M)
    return text


def lua_const_name(c_name: str) -> str:
    return c_name[3:] if c_name.startswith("LV_") else c_name


def pp_condition_from_line(line: str) -> tuple[str, str] | None:
    stripped = line.strip()
    m = re.match(r"^#\s*ifdef\s+([A-Za-z_]\w*)", stripped)
    if m:
        return "if", f"defined({m.group(1)})"
    m = re.match(r"^#\s*ifndef\s+([A-Za-z_]\w*)", stripped)
    if m:
        name = m.group(1)
        if name.endswith("_H") or name.endswith("_H_"):
            return "if", "1"
        return "if", f"!defined({name})"
    m = re.match(r"^#\s*if\s+(.+)$", stripped)
    if m:
        return "if", m.group(1).strip()
    m = re.match(r"^#\s*elif\s+(.+)$", stripped)
    if m:
        return "elif", m.group(1).strip()
    if re.match(r"^#\s*else\b", stripped):
        return "else", ""
    if re.match(r"^#\s*endif\b", stripped):
        return "endif", ""
    return None


def combine_pp_stack(stack: list[dict[str, str]]) -> str:
    parts = [frame["current"] for frame in stack if frame["current"] and frame["current"] != "1"]
    return " && ".join(f"({part})" for part in parts)


def update_pp_stack(stack: list[dict[str, str]], directive: tuple[str, str]) -> None:
    kind, cond = directive
    if kind == "if":
        stack.append({"current": cond, "seen": cond})
    elif kind == "elif":
        if not stack:
            return
        frame = stack[-1]
        previous = frame["seen"]
        frame["current"] = f"({cond}) && !({previous})" if previous else cond
        frame["seen"] = f"({previous}) || ({cond})" if previous else cond
    elif kind == "else":
        if not stack:
            return
        frame = stack[-1]
        previous = frame["seen"]
        frame["current"] = f"!({previous})" if previous else "1"
        frame["seen"] = "1"
    elif kind == "endif":
        if stack:
            stack.pop()


def collect_lvgl_constants(raw_header_texts) -> list[tuple[str, str]]:
    constants: dict[str, str] = {}
    for _path, raw_text in raw_header_texts:
        text = strip_c_comments(raw_text)
        pp_stack: list[dict[str, str]] = []
        enum_depth = 0

        for line in text.splitlines():
            directive = pp_condition_from_line(line)
            if directive is not None:
                update_pp_stack(pp_stack, directive)
                continue

            cond = combine_pp_stack(pp_stack)

            if "enum" in line and "{" in line:
                enum_depth += line.count("{") - line.count("}")
            elif enum_depth > 0:
                enum_depth += line.count("{") - line.count("}")

            if enum_depth > 0 or ("enum" in line and "{" in line):
                enum_part = line.split("//", 1)[0]
                line_enum = re.match(r"^\s*(LV_[A-Z][A-Z0-9_]*)\b\s*(?:=|,|$)", enum_part)
                if line_enum:
                    constants.setdefault(line_enum.group(1), cond)
                for enum_match in re.finditer(r"\b(LV_[A-Z][A-Z0-9_]*)\b\s*(?=[=,])", enum_part):
                    name = enum_match.group(1)
                    constants.setdefault(name, cond)

            if enum_depth < 0:
                enum_depth = 0

    return sorted(constants.items())


def normalize_type(type_name: str) -> str:
    t = type_name.strip()
    t = t.replace("struct _lv_obj_t", "lv_obj_t")
    t = re.sub(r"\bstatic\b", " ", t)
    t = re.sub(r"\binline\b", " ", t)
    t = re.sub(r"\bLV_ATTRIBUTE_[A-Z0-9_]+\b", " ", t)
    t = re.sub(r"\bLV_ATTRIBUTE_FAST_MEM\b", " ", t)
    t = t.replace("*", " * ")
    t = re.sub(r"\s+", " ", t).strip()
    t = t.replace(" *", " *")
    return t


def parse_arg(arg_src: str):
    arg = arg_src.strip()
    if not arg or arg == "void":
        return None
    if arg == "...":
        raise ValueError("varargs are not supported")
    if "(*" in arg:
        raise ValueError("function pointer args are not supported")

    # Handle array parameters first: `type name[]` -> `type *`
    arr = re.match(r"^(.*?\S)\s+([A-Za-z_]\w*)\s*\[[^\]]*\]$", arg)
    if arr:
        base_type = normalize_type(arr.group(1))
        arg_name = arr.group(2)
        type_name = normalize_type(base_type + " *")
        return type_name, arg_name

    arg = re.sub(r"\s+", " ", arg).strip()

    m = re.match(r"^(.*\S)\s+([A-Za-z_]\w*)$", arg)
    if m:
        type_name = normalize_type(m.group(1))
        arg_name = m.group(2)
    else:
        type_name = normalize_type(arg)
        arg_name = "arg"

    return type_name, arg_name


def split_args(args_src: str):
    args = []
    depth = 0
    cur = []
    for ch in args_src:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth = max(depth - 1, 0)
        elif ch == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
            continue
        cur.append(ch)
    tail = "".join(cur).strip()
    if tail:
        args.append(tail)
    return args


def resolve_alias(function_name: str, aliases: dict[str, str]) -> str:
    seen = set()
    current = function_name
    while current in aliases and current not in seen:
        seen.add(current)
        current = aliases[current]
    return current


def find_decl(function_name: str, header_texts, aliases):
    lookup_name = resolve_alias(function_name, aliases)
    pattern = re.compile(
        rf"([A-Za-z_][\w\s\*]*?)\s+{re.escape(lookup_name)}\s*\(([^;{{}}]*)\)\s*(?:;|\{{)",
        flags=re.S,
    )
    for _path, text in header_texts:
        match = pattern.search(text)
        if not match:
            continue

        ret = normalize_type(match.group(1))
        args_src = match.group(2).strip()
        args = []
        if args_src and args_src != "void":
            for raw_arg in split_args(args_src):
                parsed = parse_arg(raw_arg)
                if parsed is not None:
                    args.append(parsed)
        return ret, args
    raise ValueError(f"declaration not found for {function_name}")


def integer_like(type_name: str) -> bool:
    t = type_name.replace("const ", "", 1).strip()
    if t in INT_TYPES:
        return True
    if t.startswith("lv_") and t.endswith("_t") and t not in {"lv_color_t"}:
        return "*" not in t
    return False


def read_expr(type_name: str, arg_name: str, idx: int) -> list[str]:
    t = type_name
    base = t.replace("const ", "", 1).strip()
    if t in OBJ_PTR_TYPES:
        return [f"{t} {arg_name} = ({t}) check_obj(L, {idx});"]
    if t == "char *":
        raise ValueError("mutable char* arg is not supported")
    if t in STRING_TYPES:
        return [f"{t} {arg_name} = luaL_checkstring(L, {idx});"]
    if t in VOID_PTR_TYPES:
        return [
            f"{t} {arg_name} = nullptr;",
            f"if (lua_isstring(L, {idx})) {{",
            f"  {arg_name} = (const void *) lua_tostring(L, {idx});",
            f"}} else if (lua_islightuserdata(L, {idx})) {{",
            f"  {arg_name} = lua_touserdata(L, {idx});",
            "}",
        ]
    if base == "lv_anim_enable_t":
        return [
            f"{t} {arg_name};",
            f"if (lua_isboolean(L, {idx})) {{",
            f"  {arg_name} = lua_toboolean(L, {idx}) ? LV_ANIM_ON : LV_ANIM_OFF;",
            "} else {",
            f"  {arg_name} = ({t}) luaL_checkinteger(L, {idx});",
            "}",
        ]
    if base == "bool":
        return [f"{t} {arg_name} = ({t}) lua_toboolean(L, {idx});"]
    if base == "lv_color_t":
        return [f"{t} {arg_name} = lv_color_hex((uint32_t) luaL_checkinteger(L, {idx}));"]
    if t.endswith(" *"):
        return [f"{t} {arg_name} = ({t}) lua_touserdata(L, {idx});"]
    if integer_like(t):
        return [f"{t} {arg_name} = ({t}) luaL_checkinteger(L, {idx});"]
    raise ValueError(f"unsupported arg type: {type_name}")

def write_ret(ret_type: str, c_name: str, call_args):
    args_joined = ", ".join(call_args)
    base = ret_type.replace("const ", "", 1).strip()
    lines = []
    if base == "void":
        lines.append(f"lvgl_call_void([=]() {{ {c_name}({args_joined}); }});")
        lines.append("return 0;")
    elif ret_type in STRING_TYPES:
        lines.append(f"{ret_type} res = lvgl_call_ret([=]() -> {ret_type} {{ return {c_name}({args_joined}); }});")
        lines.append("if (res) {")
        lines.append("  lua_pushstring(L, res);")
        lines.append("} else {")
        lines.append("  lua_pushnil(L);")
        lines.append("}")
        lines.append("return 1;")
    elif base == "bool":
        lines.append(f"bool res = lvgl_call_ret([=]() -> bool {{ return {c_name}({args_joined}); }});")
        lines.append("lua_pushboolean(L, res);")
        lines.append("return 1;")
    elif base == "lv_color_t":
        lines.append(f"lv_color_t res = lvgl_call_ret([=]() -> lv_color_t {{ return {c_name}({args_joined}); }});")
        lines.append("lua_pushinteger(L, res.full);")
        lines.append("return 1;")
    elif ret_type.endswith(" *"):
        lines.append(f"{ret_type} res = lvgl_call_ret([=]() -> {ret_type} {{ return {c_name}({args_joined}); }});")
        lines.append("if (res) {")
        lines.append("  lua_pushlightuserdata(L, (void *) res);")
        lines.append("} else {")
        lines.append("  lua_pushnil(L);")
        lines.append("}")
        lines.append("return 1;")
    elif integer_like(ret_type):
        lines.append(f"{ret_type} res = lvgl_call_ret([=]() -> {ret_type} {{ return {c_name}({args_joined}); }});")
        lines.append("lua_pushinteger(L, (lua_Integer) res);")
        lines.append("return 1;")
    else:
        raise ValueError(f"unsupported return type: {ret_type}")
    return lines


def load_function_names():
    if SPEC_TXT.exists():
        names = []
        for raw in SPEC_TXT.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            names.append(line)
    elif SPEC_JSON.exists():
        with SPEC_JSON.open("r", encoding="utf-8") as f:
            spec = json.load(f)
        names = [entry["c"] for entry in spec]
    else:
        raise FileNotFoundError(f"Neither {SPEC_TXT} nor {SPEC_JSON} exists")

    deduped = []
    seen = set()
    for name in names:
        if name in seen:
            continue
        seen.add(name)
        deduped.append(name)
    return deduped


def normalize_lvgl_root(path: Path | None):
    if path is None:
        return None
    p = Path(path)
    if (p / "src").exists() and (p / "lvgl.h").exists():
        return p
    if p.name == "src" and (p / "lvgl.h").exists() and (p.parent / "lvgl.h").exists():
        return p.parent
    return None


def find_default_lvgl_root():
    candidates = [
        os.getenv("LVGL_ROOT", ""),
        str((ROOT.parents[1] / ".esphome" / "build" / "lemonade-tc" / "managed_components" / "lvgl__lvgl")),
        str((ROOT.parents[1] / ".esphome" / "build" / "lemonade-tc" / ".piolibdeps" / "lemonade-tc" / "lvgl-c")),
        str((ROOT.parents[1] / "misc" / "lvgl8")),
    ]
    for c in candidates:
        if not c:
            continue
        normalized = normalize_lvgl_root(Path(c))
        if normalized is not None:
            return normalized
    return None


def collect_headers(lvgl_root: Path):
    headers = [lvgl_root / "lvgl.h"]
    headers.extend(
        sorted(
            header for header in (lvgl_root / "src").rglob("*.h")
            if not any(part in HEADER_SKIP_PARTS for part in header.parts)
        )
    )
    out = []
    aliases = {}
    for header in headers:
        if not header.exists():
            continue
        raw_text = header.read_text(encoding="utf-8", errors="ignore")
        aliases.update(parse_alias_macros(raw_text))
        text = strip_comments(raw_text)
        out.append((header, text))
    return out, aliases


def collect_included_headers_for_constants(lvgl_root: Path):
    seen: set[Path] = set()
    out = []

    def visit(header: Path) -> None:
        header = header.resolve()
        if header in seen or not header.exists():
            return
        seen.add(header)
        if any(part in CONSTANT_HEADER_SKIP_PARTS for part in header.parts) or "private" in header.name:
            return

        raw_text = header.read_text(encoding="utf-8", errors="ignore")
        out.append((header, raw_text))
        text = strip_c_comments(raw_text)
        for match in re.finditer(r"^\s*#\s*include\s+\"([^\"]+)\"", text, flags=re.M):
            include_path = (header.parent / match.group(1)).resolve()
            try:
                include_path.relative_to(lvgl_root.resolve())
            except ValueError:
                continue
            visit(include_path)

    visit(lvgl_root / "lvgl.h")
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate Lua LVGL bindings")
    parser.add_argument("--lvgl-root", type=Path, default=None, help="Path to LVGL root (contains lvgl.h and src/)")
    parser.add_argument("--strict", action="store_true", help="Fail on any unsupported/missing function")
    cli_args = parser.parse_args()

    lvgl_root = normalize_lvgl_root(cli_args.lvgl_root) if cli_args.lvgl_root else find_default_lvgl_root()
    if lvgl_root is None:
        print("LVGL root not found. Pass --lvgl-root or set LVGL_ROOT.", file=sys.stderr)
        return 2

    header_texts, aliases = collect_headers(lvgl_root)
    names = load_function_names()
    constants = collect_lvgl_constants(collect_included_headers_for_constants(lvgl_root))

    lines = []
    lines.append("// Auto-generated by tools/gen_lvgl_bindings.py. Do not edit manually.")
    lines.append("#pragma once")
    lines.append("#ifdef LUA_LVGL_IMPL")
    lines.append("")
    bound = []
    skipped = []

    for c_name in names:
        if c_name in MANUAL_BINDINGS:
            continue
        lua_name = c_name[3:] if c_name.startswith("lv_") else c_name
        try:
            ret_type, fn_args = find_decl(c_name, header_texts, aliases)
            fn_lines = []
            fn_lines.append(f"static int l_{lua_name}(lua_State *L) {{")

            arg_names = []
            obj_guard_names = []
            for idx, (type_name, source_arg_name) in enumerate(fn_args, start=1):
                name = f"a{idx - 1}_{source_arg_name}"
                expr_lines = read_expr(type_name, name, idx)
                for row in expr_lines:
                    fn_lines.append(f"  {row}")
                if type_name in OBJ_PTR_TYPES:
                    obj_guard_names.append(name)
                arg_names.append(name)

            if obj_guard_names:
                guard = " || ".join(f"{n} == nullptr" for n in obj_guard_names)
                fn_lines.append(f"  if ({guard}) {{")
                if ret_type.replace("const ", "", 1).strip() == "void":
                    fn_lines.append("    return 0;")
                else:
                    fn_lines.append("    lua_pushnil(L);")
                    fn_lines.append("    return 1;")
                fn_lines.append("  }")

            for row in write_ret(ret_type, c_name, arg_names):
                fn_lines.append(f"  {row}")
            fn_lines.append("}\n")

            lines.extend(fn_lines)
            bound.append(lua_name)
        except Exception as e:
            skipped.append(f"{c_name}: {e}")

    if cli_args.strict and skipped:
        print("Binding generation failed:", file=sys.stderr)
        for err in skipped:
            print(f"  - {err}", file=sys.stderr)
        return 1

    if not bound:
        print("Binding generation failed: no functions generated", file=sys.stderr)
        for err in skipped:
            print(f"  - {err}", file=sys.stderr)
        return 1

    lines.append("static void register_lvgl_gen(lua_State *L) {")
    for lua_name in bound:
        lines.append(f"  lua_pushcfunction(L, l_{lua_name});")
        lines.append(f"  lua_setfield(L, -2, \"{lua_name}\");")
    if constants:
        lines.append("")
        lines.append("  // LVGL integer macros and enum constants")
    for c_name, cond in constants:
        lua_name = lua_const_name(c_name)
        if cond:
            lines.append(f"#if {cond}")
        lines.append(f"  set_int_field(L, \"{lua_name}\", {c_name});")
        if cond:
            lines.append("#endif")
    lines.append("}")
    lines.append("")
    lines.append("#endif  // LUA_LVGL_IMPL")

    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Generated {OUT} ({len(bound)} functions), LVGL root: {lvgl_root}")
    if skipped:
        print(f"Skipped {len(skipped)} unsupported functions:")
        for err in skipped:
            print(f"  - {err}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
