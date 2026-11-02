#!/bin/sh
# Write a recomp module's release into core/deadcast/catalog.h, replacing the module's entry if it has one.
#   shell/brick/catalog.sh ARCHIVE URL       ARCHIVE: the module's tarball; URL: where it is released
set -e
cd "$(dirname "$0")/../.."
ARCHIVE=${1:?usage: shell/brick/catalog.sh ARCHIVE URL}
URL=${2:?usage: shell/brick/catalog.sh ARCHIVE URL}
python3 -I - "$ARCHIVE" "$URL" core/deadcast/catalog.h <<'EOF'
import hashlib, json, sys, tarfile

archive, url, catalog = sys.argv[1:4]
data = open(archive, "rb").read()
with tarfile.open(archive) as tar:
    manifest = tar.extractfile("module.cfg").read().decode()
    store = json.load(tar.extractfile("store.json"))
cfg, discs = {}, []
for line in manifest.splitlines():
    line = line.strip()
    if "=" in line and not line.startswith("#"):
        k, v = (s.strip() for s in line.split("=", 1))
        if k == "disc":
            discs.append(v)
        else:
            cfg.setdefault(k, v)
info = json.dumps(store, ensure_ascii=False, separators=(",", ":"))
listed = ", ".join('"' + d + '"' for d in discs)
entry = (f'\t\t{{ "{cfg["id"]}", "{cfg["name"]}", "{cfg["version"]}", "{url}", {len(data)}, '
         f'"{hashlib.sha256(data).hexdigest()}", "{cfg.get("title", "")}", R"json({info})json", "{cfg.get("needs", "")}", '
         f'{{ {listed} }} }},\n')
lines = open(catalog, encoding="utf-8").read().splitlines(keepends=True)
start = lines.index("\treturn {\n") + 1
end = lines.index("\t};\n")
kept = [l for l in lines[start:end] if not l.lstrip().startswith(f'{{ "{cfg["id"]}",')]
open(catalog, "w", encoding="utf-8").write("".join(lines[:start] + kept + [entry] + lines[end:]))
print(f"catalog: {cfg['id']} {cfg['version']}, {len(data)} bytes")
EOF
