#!/usr/bin/env python3
"""
Enforces BrewDial's architecture and wire-format rules (see CLAUDE.md). Runs before every
PlatformIO build (extra_scripts = pre:tools/check_rules.py), as part of the CMake host build,
and standalone:  python tools/check_rules.py

Exit code / build failure on any violation. Keep the rules here and in CLAUDE.md in sync.
"""
import os
import re
import sys

STD_HEADER = re.compile(r"^<[a-z_]+(\.h)?>$")

# --------------------------------------------------------------------------- layering
# For each library: which quoted/angled includes are allowed. Anything that matches a
# FORBIDDEN pattern is an error. Std headers (<cstdint>, <cmath>, <atomic>...) are always fine.
PLATFORM = [r"^esp_", r"^freertos/", r"^driver/", r"^soc/", r"^hal/", r"^nvs", r"^Arduino", r"^Wire\.h",
            r"^sdkconfig"]
UI = [r"^lvgl\.h", r"^brew_ui\.h", r"^brew_bg\.h", r"^fonts/", r"^SDL"]
APP = [r"^brew/app/"]
DRIVERS = [r"^brew/drivers/(?!interfaces/)"]   # driver implementations (chips, esp32, ...)
INTERFACES = [r"^brew/drivers/interfaces/"]
NETWORK = [r"^brew/network/"]
CORE = [r"^brew/[^/]+\.h$"]                   # brew/<x>.h

LAYERS = {
    "lib/drivers/interfaces": PLATFORM + UI + APP + DRIVERS + CORE + NETWORK,
    "lib/core":               PLATFORM + UI + APP + DRIVERS + NETWORK,
    "lib/network":            PLATFORM + UI + APP + DRIVERS + CORE,
    "lib/drivers/chips":      PLATFORM + UI + APP + CORE + NETWORK + [r"^brew/drivers/esp32/"],
    "lib/drivers/esp32":      UI + APP + CORE + NETWORK + [r"^brew/drivers/chips/", r"^Arduino", r"^Wire\.h"],
    "lib/app":                DRIVERS + [r"^Arduino", r"^Wire\.h"],
    "lib/ui":                 APP + DRIVERS + CORE + INTERFACES + NETWORK + PLATFORM[:-1],
}
# lib/ui may use esp_heap_caps.h for its PSRAM allocator (guarded by ESP_PLATFORM).
LAYER_EXCEPTIONS = {("lib/ui", "esp_heap_caps.h")}

# Stricter rules for single files inside a library. The UI model is shared by every layout
# and unit-tested on the host, so it must not see LVGL, a layout, or the platform.
FILE_RULES = [
    (re.compile(r"^lib/ui/src/(brew_model\.[ch]|brew_ui_types\.h)$"),
     "the UI model", UI + [r"^brew_layout\.h", r"^layout_"] + PLATFORM),
]

# --------------------------------------------------------------------------- wire format
CODEC_FILES = re.compile(r"lib/core/src/codec[^/]*\.cpp$")
WIRE_HEADER = re.compile(r'#\s*include\s*"(wire_v\d+\.h)"')
MSG_TYPES = r"(?:brew::)?(?:State|Command|HistoryChunk|Message)"
WIRE_SMELLS = [
    (re.compile(r"#\s*pragma\s+pack"), "packed struct (#pragma pack)"),
    (re.compile(r"__attribute__\s*\(\(\s*packed"), "packed struct (__attribute__((packed)))"),
    (re.compile(r"\b__packed\b"), "packed struct (__packed)"),
    (re.compile(r"sizeof\s*\(\s*" + MSG_TYPES + r"\s*\)"),
     "sizeof(<message type>): message structs must not be treated as byte blobs"),
    (re.compile(r"\(\s*(?:const\s+)?(?:u?int8_t|char|unsigned\s+char|std::byte)\s*\*\s*\)\s*&"),
     "casting an object's address to bytes (serialise field by field in codec.cpp)"),
    (re.compile(r"reinterpret_cast\s*<\s*(?:const\s+)?(?:u?int8_t|char|unsigned\s+char|std::byte)\s*\*\s*>\s*\(\s*&"),
     "reinterpret_cast of an object's address to bytes (serialise field by field in codec.cpp)"),
    (re.compile(r"\bnvs_set_blob\b|\bnvs_get_blob\b|\bsetBlob\b|\bputBytes\b|\bgetBytes\b"),
     "raw blob storage: persist settings as typed key/value pairs"),
]

SRC_EXT = (".h", ".hpp", ".c", ".cpp", ".cc", ".ino")
SCAN_DIRS = ("lib", "boards", "sim", "test")
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)


def rel(root, p):
    return os.path.relpath(p, root).replace(os.sep, "/")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def source_files(root):
    for d in SCAN_DIRS:
        base = os.path.join(root, d)
        for dp, dns, fns in os.walk(base):
            dns[:] = [x for x in dns if x not in ("build", ".pio", "_deps", "out")]
            for f in fns:
                if f.endswith(SRC_EXT):
                    yield os.path.join(dp, f)


def check_layers(root, path, text, errors):
    r = rel(root, path)
    for lib, forbidden in LAYERS.items():
        if not r.startswith(lib + "/"):
            continue
        for m in INCLUDE.finditer(text):
            inc = m.group(2)
            if (lib, inc) in LAYER_EXCEPTIONS:
                continue
            if m.group(1) == "<" and STD_HEADER.match("<" + inc + ">") and "/" not in inc:
                continue
            for pat in forbidden:
                if re.search(pat, inc):
                    line = text.count("\n", 0, m.start()) + 1
                    errors.append(f"{r}:{line}: layering: {lib} must not include \"{inc}\"")
                    break
    for rx, what, forbidden in FILE_RULES:
        if not rx.search(r):
            continue
        for m in INCLUDE.finditer(text):
            inc = m.group(2)
            if m.group(1) == "<" and STD_HEADER.match("<" + inc + ">") and "/" not in inc:
                continue
            if any(re.search(pat, inc) for pat in forbidden):
                line = text.count("\n", 0, m.start()) + 1
                errors.append(f"{r}:{line}: layering: {what} must not include \"{inc}\" (no LVGL, layouts or platform)")


def check_wire(root, path, text, errors):
    r = rel(root, path)
    is_codec = bool(CODEC_FILES.search(r))
    code = strip_comments(text)
    for m in WIRE_HEADER.finditer(code):
        if not is_codec:
            line = code.count("\n", 0, m.start()) + 1
            errors.append(f"{r}:{line}: wire: {m.group(1)} is private to lib/core/src/codec*.cpp")
    if is_codec or r.startswith("test/"):
        return
    for pat, why in WIRE_SMELLS:
        for m in pat.finditer(code):
            line = code.count("\n", 0, m.start()) + 1
            errors.append(f"{r}:{line}: wire: {why}")


def parse_proto(path):
    msgs, cur = {}, None
    for raw in open(path, encoding="utf-8"):
        line = raw.split("//")[0].strip()
        m = re.match(r"message\s+(\w+)\s*\{", line)
        if m:
            cur = m.group(1)
            msgs[cur] = {}
            continue
        if line.startswith("}"):
            cur = None
            continue
        m = re.match(r"(?:repeated\s+)?\w+\s+(\w+)\s*=\s*(\d+)\s*;", line)
        if cur and m:
            msgs[cur][int(m.group(2))] = m.group(1)
    return msgs


def parse_wire_doc(path):
    msgs, cur = {}, None
    for raw in open(path, encoding="utf-8"):
        m = re.match(r"\s*\*\s+(\w+)\s+\(msg_type \d+\)", raw)
        if m:
            cur = m.group(1)
            msgs[cur] = {}
            continue
        m = re.match(r"\s*\*\s+#(\d+)\s+(\w+)", raw)
        if cur and m:
            msgs[cur][int(m.group(1))] = m.group(2)
    return msgs


def check_schema(root, errors):
    proto = os.path.join(root, "proto", "brewdial.proto")
    wires = sorted(f for f in os.listdir(os.path.join(root, "lib", "core", "src")) if re.match(r"wire_v\d+\.h$", f))
    if not os.path.exists(proto) or not wires:
        errors.append("schema: proto/brewdial.proto or lib/core/src/wire_v*.h missing")
        return
    want = parse_proto(proto)
    for w in wires:
        have = parse_wire_doc(os.path.join(root, "lib", "core", "src", w))
        for msg, fields in want.items():
            if msg not in have:
                errors.append(f"schema: {w} does not document message {msg} from brewdial.proto")
                continue
            for num, name in fields.items():
                if have[msg].get(num) != name:
                    errors.append(f"schema: {msg} field #{num} is '{name}' in brewdial.proto but "
                                  f"'{have[msg].get(num)}' in {w}")
            for num in have[msg]:
                if num not in fields:
                    errors.append(f"schema: {msg} field #{num} in {w} is missing from brewdial.proto")


# --------------------------------------------------------------------------- LVGL pitfalls
# lv_arc_set_angles() moves the end first: moving an arc backwards, the end passes the start for
# a moment, the arc looks like a full circle and LVGL repaints its whole box (the whole dial,
# every knob step: 6-9 fps turning down vs 21+ up). kit_arc_set_angles() moves the leading edge
# first. See CLAUDE.md, "LVGL redraw cost".
LVGL_PITFALLS = [
    (re.compile(r"\blv_arc_set_angles\s*\("), "lv_arc_set_angles() repaints the whole arc when it moves "
     "backwards; use kit_arc_set_angles() (layout_kit.h)"),
]
LVGL_PITFALL_HOME = re.compile(r"^lib/ui/src/layout_kit\.c$")   # the helper itself


def check_lvgl(root, path, text, errors):
    r = rel(root, path)
    if LVGL_PITFALL_HOME.search(r):
        return
    code = strip_comments(text)
    for pat, why in LVGL_PITFALLS:
        for m in pat.finditer(code):
            line = code.count("\n", 0, m.start()) + 1
            errors.append(f"{r}:{line}: lvgl: {why}")


def run(root):
    errors = []
    for p in source_files(root):
        with open(p, encoding="utf-8", errors="replace") as f:
            text = f.read()
        check_layers(root, p, text, errors)
        check_wire(root, p, text, errors)
        check_lvgl(root, p, text, errors)
    check_schema(root, errors)
    check_generated(root, errors)
    return errors


def check_generated(root, errors):
    """Generated sources must match what they're generated from (so an edit isn't silently lost)."""
    sys.path.insert(0, os.path.join(root, "tools"))
    try:
        import gen_setup_page
        if not gen_setup_page.is_current():
            errors.append("generated: lib/network/src/setup_page.cpp is out of date with "
                          "lib/network/web/setup.html: run  python tools/gen_setup_page.py")
    finally:
        sys.path.pop(0)


def main(root):
    errors = run(root)
    if errors:
        print("\nBrewDial rule check FAILED (see CLAUDE.md):", file=sys.stderr)
        for e in errors:
            print("  " + e, file=sys.stderr)
        print("", file=sys.stderr)
        return 1
    print("BrewDial rule check: OK")
    return 0


try:
    Import("env")  # noqa: F821  (PlatformIO / SCons)
    _root = env.subst("$PROJECT_DIR")  # noqa: F821
    if main(_root) != 0:
        env.Exit(1)  # noqa: F821
except NameError:
    if __name__ == "__main__":
        sys.exit(main(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
