#!/bin/sh
# Prepare a built recomp module (shell/brick/recomp.sh: build-brick/recomp/MODULE) for a Deadcast
# release: packed as build-brick/release/recomp-MODULE-VERSION.tar.gz, and frontend/catalog.h written
# with its size, SHA-256 and discs and the release TAG it is to be attached to. Then build Deadcast
# (shell/brick/build.sh): that build offers the module, and installs it only if it is this very file.
# Attaching the file to the release on GitHub is left to whoever publishes it.
#   shell/brick/recomp-release.sh MODULE TAG        (TAG: the Deadcast release, e.g. 1.1.0)
set -e
cd "$(dirname "$0")/../.."
M=${1:?usage: shell/brick/recomp-release.sh MODULE TAG}
TAG=${2:?usage: shell/brick/recomp-release.sh MODULE TAG}
SRC=build-brick/recomp/$M
[ -f "$SRC/module.cfg" ] || { echo "recomp-release: no $SRC (shell/brick/recomp.sh $M DISC)"; exit 2; }
mkdir -p build-brick/release
python3 - "$SRC" "$TAG" frontend/catalog.h build-brick/release <<'EOF'
import hashlib, io, os, re, sys, tarfile

src, tag, catalog, out = sys.argv[1:5]
cfg = {}
discs = []
for line in open(os.path.join(src, "module.cfg"), encoding="utf-8"):
    line = line.strip()
    if not line or line.startswith("#") or "=" not in line:
        continue
    k, v = (s.strip() for s in line.split("=", 1))
    if k == "disc":
        discs.append(v)
    else:
        cfg.setdefault(k, v)
mid, name, version = cfg["id"], cfg["name"], cfg["version"]
if len(discs) > 4:
    sys.exit("recomp-release: a catalog entry holds 4 discs at most")
file = f"recomp-{mid}-{version}.tar.gz"
path = os.path.join(out, file)

# the module's files at the archive's root, in a fixed order with fixed times and owners, so the same
# build packs to the same bytes
def normal(info):
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    info.mtime = 0
    info.mode = 0o755 if info.isdir() or os.access(os.path.join(src, info.name), os.X_OK) else 0o644
    return info
with open(path, "wb") as raw:
    import gzip
    with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=9) as gz:
        with tarfile.open(fileobj=gz, mode="w", format=tarfile.USTAR_FORMAT) as tar:
            for root, dirs, files in os.walk(src):
                dirs.sort()
                rel = os.path.relpath(root, src)
                if rel != ".":
                    tar.add(root, arcname=rel, recursive=False, filter=normal)
                for f in sorted(files):
                    p = os.path.join(root, f)
                    tar.add(p, arcname=os.path.normpath(os.path.join(rel, f)), filter=normal)
data = open(path, "rb").read()
sha = hashlib.sha256(data).hexdigest()

title = cfg.get("title", "")
entry = (f'\t{{ "{mid}", "{name}", "{version}", "{file}", {len(data)}, "{sha}", "{title}",\n'
         f'\t\t{{ {", ".join(chr(34) + d + chr(34) for d in discs)} }} }},\n')
text = open(catalog, encoding="utf-8").read()
text = re.sub(r'(constexpr const char \*Release = ")[^"]*(";)',
              lambda m: m.group(1) + f"https://github.com/tyrannotorus/emulator-dreamcast-deadcast/releases/download/{tag}/" + m.group(2), text)
start = text.index("constexpr CatalogEntry Catalog[] = {\n") + len("constexpr CatalogEntry Catalog[] = {\n")
end = text.index("};", start)
lines = [l + "\n" for l in text[start:end].split("\n") if l.strip()]
# this module's entry replaced (or the placeholder of an empty catalog), the others kept
kept, i = [], 0
while i < len(lines):
    l = lines[i]
    if l.lstrip().startswith(f'{{ "{mid}",') or "none yet" in l:
        i += 2 if l.lstrip().startswith("{ \"") else 1
        continue
    kept.append(l)
    i += 1
text = text[:start] + "".join(kept) + entry + text[end:]
open(catalog, "w", encoding="utf-8").write(text)
print(f"recomp-release: {path} ({len(data)} bytes, SHA-256 {sha})")
print(f"recomp-release: frontend/catalog.h: {mid} {version}, release {tag}")
EOF
