#!/usr/bin/env python3
# Build the store's list from the latest release of each repository in the registry. A release's module is its
# recomp-*.tar.gz, with module.cfg and store.json at the archive's root. A module whose release cannot be read
# keeps its last entry while that entry's file is still there, and fails the run once the list is written, so
# GitHub says so. Nothing from a release is run.
#   build-list.py REGISTRY LIST       LIST: read for the last entries, then written. GH_TOKEN: for the GitHub API
import hashlib, io, json, os, re, sys, tarfile, urllib.error, urllib.request

registry, listing = sys.argv[1:3]
token = os.environ.get("GH_TOKEN", "")
MAX_ARCHIVE = 1 << 30
MAX_TEXT = 1 << 20


def request(url, api):
    headers = {"User-Agent": "deadcast-store"}
    if api:
        headers["Accept"] = "application/vnd.github+json"
        if token:
            headers["Authorization"] = "Bearer " + token
    return urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=120)


def gone(url):
    """its file is known to be removed: not merely out of reach"""
    try:
        req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": "deadcast-store"})
        urllib.request.urlopen(req, timeout=60).close()
        return False
    except urllib.error.HTTPError as error:
        return error.code in (404, 410)
    except OSError:
        return False


def read_list(path):
    """entries as lists of (key, value), by url"""
    entries, entry = {}, None
    try:
        lines = open(path, encoding="utf-8").read().splitlines()
    except OSError:
        return entries
    for line in lines:
        if "=" not in line:
            continue
        k, v = (s.strip() for s in line.split("=", 1))
        if k == "module":
            entry = []
        if entry is None:
            continue
        entry.append((k, v))
        if k == "url":
            entries[v] = entry
    return entries


def module_entry(asset):
    """an entry from the asset's archive"""
    with request(asset["browser_download_url"], False) as response:
        data = response.read(MAX_ARCHIVE + 1)
    if len(data) > MAX_ARCHIVE:
        raise ValueError("archive too large")
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        manifest = tar.extractfile("module.cfg").read(MAX_TEXT).decode("utf-8")
        store = json.loads(tar.extractfile("store.json").read(MAX_TEXT))
    cfg, discs = {}, []
    for line in manifest.splitlines():
        line = line.strip()
        if "=" not in line or line.startswith("#"):
            continue
        k, v = (s.strip() for s in line.split("=", 1))
        if k == "disc":
            discs.append(v)
        else:
            cfg.setdefault(k, v)
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", cfg.get("id", "")):
        raise ValueError("id is not a plain name")
    for key in ("name", "version"):
        if not cfg.get(key):
            raise ValueError("no " + key)
    if not discs:
        raise ValueError("no disc")
    entry = [("module", cfg["id"]), ("name", cfg["name"]), ("title", cfg.get("title", "")),
             ("version", cfg["version"]), ("url", asset["browser_download_url"]), ("size", str(len(data))),
             ("sha256", hashlib.sha256(data).hexdigest())]
    if cfg.get("needs"):
        entry.append(("needs", cfg["needs"]))
    entry.append(("info", json.dumps(store, ensure_ascii=False, separators=(",", ":"))))
    entry += [("disc", d) for d in discs]
    return entry


last = read_list(listing)
entries = {}
failed = 0
for repo in open(registry, encoding="utf-8").read().splitlines():
    repo = repo.strip()
    if not repo or repo.startswith("#"):
        continue
    kept = [e for url, e in last.items() if url.startswith(f"https://github.com/{repo}/")]
    try:
        if not re.fullmatch(r"[\w.-]+/[\w.-]+", repo):
            raise ValueError("not owner/name")
        with request(f"https://api.github.com/repos/{repo}/releases/latest", True) as response:
            release = json.load(response)
        assets = [a for a in release["assets"] if re.fullmatch(r"recomp-.*\.tar\.gz", a["name"])]
        if len(assets) != 1:
            raise ValueError("not one recomp-*.tar.gz in its latest release")
        known = last.get(assets[0]["browser_download_url"])
        digest = assets[0].get("digest") or ""
        # read again only if it is new, or its file was replaced
        if known and (not digest or digest == "sha256:" + dict(known)["sha256"]):
            entry = known
        else:
            entry = module_entry(assets[0])
    except (OSError, ValueError, KeyError, AttributeError, tarfile.TarError, urllib.error.URLError) as error:
        if not kept:
            print(f"::error::{repo}: {error}: not listed")
            failed += 1
            continue
        if gone(dict(kept[0])["url"]):
            print(f"::warning::{repo}: {error}: its file was removed: no longer listed")
            continue
        print(f"::error::{repo}: {error}: its last entry stays listed")
        failed += 1
        entry = kept[0]
    mid = dict(entry)["module"]
    if mid in entries:
        print(f"::error::{repo}: id {mid} is taken: not listed")
        failed += 1
        continue
    entries[mid] = entry
    print(f"{repo}: {mid} {dict(entry)['version']}")

with open(listing, "w", encoding="utf-8") as out:
    out.write("store = 1\n")    # a list with nothing on it is still a list
    for mid in sorted(entries):
        for k, v in entries[mid]:
            if v:
                out.write(f"{k} = {v}\n")

sys.exit(1 if failed else 0)
