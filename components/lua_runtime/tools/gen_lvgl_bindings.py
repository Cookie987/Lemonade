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


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//.*?$", " ", text, flags=re.M)
    text = re.sub(r"^\s*#.*?$", " ", text, flags=re.M)
    return text


def normalize_type(type_name: str) -> str:
    t = type_name.strip()
    t = t.replace("struct _lv_obj_t", "lv_obj_t")
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


def find_decl(function_name: str, header_texts):
    pattern = re.compile(
        rf"([A-Za-z_][\w\s\*]*?)\s+{re.escape(function_name)}\s*\(([^;{{}}]*)\)\s*;",
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


def find_default_lvgl_root():
    candidates = [
        os.getenv("LVGL_ROOT", ""),
        str((ROOT.parents[1] / ".esphome" / "build" / "lemonade-tc" / ".piolibdeps" / "lemonade-tc" / "lvgl-c")),
        str((ROOT.parents[1] / "misc" / "lvgl8")),
    ]
    for c in candidates:
        if not c:
            continue
        p = Path(c)
        if (p / "src").exists() and (p / "lvgl.h").exists():
            return p
    return None


def collect_headers(lvgl_root: Path):
    headers = [lvgl_root / "lvgl.h"]
    headers.extend(sorted((lvgl_root / "src").rglob("*.h")))
    out = []
    for header in headers:
        if not header.exists():
            continue
        text = strip_comments(header.read_text(encoding="utf-8", errors="ignore"))
        out.append((header, text))
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate Lua LVGL bindings")
    parser.add_argument("--lvgl-root", type=Path, default=None, help="Path to LVGL root (contains lvgl.h and src/)")
    parser.add_argument("--strict", action="store_true", help="Fail on any unsupported/missing function")
    cli_args = parser.parse_args()

    lvgl_root = cli_args.lvgl_root or find_default_lvgl_root()
    if lvgl_root is None:
        print("LVGL root not found. Pass --lvgl-root or set LVGL_ROOT.", file=sys.stderr)
        return 2

    header_texts = collect_headers(lvgl_root)
    names = load_function_names()

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
            ret_type, fn_args = find_decl(c_name, header_texts)
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
