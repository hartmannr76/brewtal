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
DRIVERS = [r"^brew/drivers/"]
CORE = [r"^brew/(?!hal/)[^/]+\.h$"]   # brew/<x>.h that is not brew/hal/...

LAYERS = {
    "lib/brew_hal":           PLATFORM + UI + APP + DRIVERS + CORE,
    "lib/brew_core":          PLATFORM + UI + APP + DRIVERS,
    "lib/brew_drivers":       PLATFORM + UI + APP + CORE + [r"^brew/drivers/esp"],
    "lib/brew_drivers_esp32": UI + APP + CORE + [r"^Arduino", r"^Wire\.h"],
    "lib/brew_app":           DRIVERS + [r"^Arduino", r"^Wire\.h"],
    "lib/brew_ui":            APP + DRIVERS + CORE + [r"^brew/hal/"] + PLATFORM[:-1],
}
# brew_ui may use esp_heap_caps.h for its PSRAM allocator (guarded by ESP_PLATFORM).
LAYER_EXCEPTIONS = {("lib/brew_ui", "esp_heap_caps.h")}

# Stricter rules for single files inside a library. The UI model is shared by every layout
# and unit-tested on the host, so it must not see LVGL, a layout, or the platform.
FILE_RULES = [
    (re.compile(r"^lib/brew_ui/src/(brew_model\.[ch]|brew_ui_types\.h)$"),
     "the UI model", UI + [r"^brew_layout\.h", r"^layout_"] + PLATFORM),
]

# --------------------------------------------------------------------------- wire format
CODEC_FILES = re.compile(r"lib/brew_core/src/codec[^/]*\.cpp$")
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
        if lib == "lib/brew_drivers" and r.startswith("lib/brew_drivers_esp32/"):
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
            errors.append(f"{r}:{line}: wire: {m.group(1)} is private to lib/brew_core/src/codec*.cpp")
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
    wires = sorted(f for f in os.listdir(os.path.join(root, "lib", "brew_core", "src")) if re.match(r"wire_v\d+\.h$", f))
    if not os.path.exists(proto) or not wires:
        errors.append("schema: proto/brewdial.proto or lib/brew_core/src/wire_v*.h missing")
        return
    want = parse_proto(proto)
    for w in wires:
        have = parse_wire_doc(os.path.join(root, "lib", "brew_core", "src", w))
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


def run(root):
    errors = []
    for p in source_files(root):
        with open(p, encoding="utf-8", errors="replace") as f:
            text = f.read()
        check_layers(root, p, text, errors)
        check_wire(root, p, text, errors)
    check_schema(root, errors)
    return errors


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
