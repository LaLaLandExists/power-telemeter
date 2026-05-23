Import("env")
import atexit, gzip, os, shutil, sys

# Import vendored minifiers from the same scripts/ directory.
sys.path.insert(0, os.path.join(env.subst("$PROJECT_DIR"), "scripts"))
from minify import jsmin, cssmin, htmlmin

# Files kept in data/ for local dev (Python http.server) but excluded from
# LittleFS entirely -- never compressed, never packed into the image.
EXCLUDE = {"simulation.js"}

# Files that are already minified or should not be minified (e.g. libraries,
# JSON manifests).  They are still gzip-compressed for the image.
SKIP_MINIFY = {"chart.js", "site.webmanifest"}

# Extension -> minifier mapping.
MINIFIERS = {
    ".js":   jsmin,
    ".css":  cssmin,
    ".html": htmlmin,
}

data_dir    = os.path.join(env.subst("$PROJECT_DIR"), "data")
project_dir = env.subst("$PROJECT_DIR")
stash_dir   = os.path.join(project_dir, ".fsbuild_stash")

# (stash_path, restore_path) -- populated as files are moved out.
_stash_log = []


def _stash(src, stash_name):
    os.makedirs(stash_dir, exist_ok=True)
    dst = os.path.join(stash_dir, stash_name)
    shutil.move(src, dst)
    _stash_log.append((dst, src))


def _restore_all():
    for stash_path, restore_path in _stash_log:
        if os.path.isfile(stash_path):
            shutil.move(stash_path, restore_path)
    try:
        if os.path.isdir(stash_dir) and not os.listdir(stash_dir):
            os.rmdir(stash_dir)
    except OSError:
        pass


# Restore originals after the LittleFS image is built (normal SCons exit).
atexit.register(_restore_all)

# --- Step 1: remove EXCLUDED files so the LittleFS builder never sees them --
for fname in EXCLUDE:
    src = os.path.join(data_dir, fname)
    if os.path.isfile(src):
        _stash(src, fname)
        print(f"  skip  {fname}  (excluded from LittleFS -- dev only)")
    gz = os.path.join(data_dir, fname + ".gz")
    if os.path.isfile(gz):
        os.remove(gz)

# --- Step 2: minify eligible files in-place (originals saved to stash) ------
for fname in sorted(os.listdir(data_dir)):
    if fname.endswith(".gz") or fname in EXCLUDE or fname in SKIP_MINIFY:
        continue
    _, ext = os.path.splitext(fname)
    minify_fn = MINIFIERS.get(ext)
    if minify_fn is None:
        continue
    src = os.path.join(data_dir, fname)
    if not os.path.isfile(src):
        continue
    with open(src, "r", encoding="utf-8") as fh:
        original = fh.read()
    minified = minify_fn(original)
    _stash(src, fname + ".orig")          # save original before overwriting
    with open(src, "w", encoding="utf-8", newline="") as fh:
        fh.write(minified)
    orig_kb = len(original.encode()) / 1024
    mini_kb = len(minified.encode()) / 1024
    saved   = orig_kb - mini_kb
    print(f"  min   {fname:30s}  {orig_kb:6.1f} KB -> {mini_kb:5.1f} KB  (-{saved:.1f} KB)")

# --- Step 3: remove stale .gz artifacts whose source no longer exists -------
for fname in os.listdir(data_dir):
    if not fname.endswith(".gz"):
        continue
    gz_path  = os.path.join(data_dir, fname)
    src_path = gz_path[:-3]
    if not os.path.isfile(src_path):
        os.remove(gz_path)
        print(f"  del   {fname}  (source removed)")

# --- Step 4: gzip all remaining data files (minified where applicable) ------
for fname in sorted(os.listdir(data_dir)):
    if fname.endswith(".gz") or fname in EXCLUDE:
        continue
    src = os.path.join(data_dir, fname)
    if not os.path.isfile(src):
        continue
    dst = src + ".gz"
    with open(src, "rb") as f_in, gzip.open(dst, "wb", compresslevel=9) as f_out:
        shutil.copyfileobj(f_in, f_out)
    src_kb = os.path.getsize(src) / 1024
    dst_kb = os.path.getsize(dst) / 1024
    print(f"  gzip  {fname:30s}  {src_kb:6.1f} KB -> {dst_kb:5.1f} KB")
