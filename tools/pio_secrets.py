"""PlatformIO pre-script: reads secrets/brewfather.env (git-ignored) into string macros
BF_USER_ID, BF_API_KEY, BF_STREAM_ID for builds that talk to Brewfather. Only envs that list it
in extra_scripts get the values; nothing is written to the repo or printed.
"""
import os

Import("env")  # noqa: F821  (PlatformIO SCons)

path = os.path.join(env.subst("$PROJECT_DIR"), "secrets", "brewfather.env")  # noqa: F821
if not os.path.exists(path):
    raise SystemExit("secrets/brewfather.env is missing: copy secrets/brewfather.env.example and fill it in")

vals = {}
with open(path, encoding="utf-8") as f:
    for line in f:
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, _, v = line.partition("=")
        vals[k.strip()] = v.strip().strip('"').strip("'")

stream = vals.get("BF_STREAM_ID", "")
if "id=" in stream:   # the whole logging URL was pasted
    vals["BF_STREAM_ID"] = stream.split("id=", 1)[1].split("&", 1)[0]

for key in ("BF_USER_ID", "BF_API_KEY", "BF_STREAM_ID"):
    if not vals.get(key):
        raise SystemExit(f"{key} is empty in secrets/brewfather.env")
    env.Append(CPPDEFINES=[(key, env.StringifyMacro(vals[key]))])  # noqa: F821
