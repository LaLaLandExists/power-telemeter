Import("env")
import gzip, os, shutil

def gzip_data_files(source, target, env):
    data_dir = os.path.join(env.subst("$PROJECT_DIR"), "data")

    # Remove .gz files whose source no longer exists (stale artifacts).
    for fname in os.listdir(data_dir):
        if not fname.endswith(".gz"):
            continue
        gz_path  = os.path.join(data_dir, fname)
        src_path = gz_path[:-3]  # strip .gz
        if not os.path.isfile(src_path):
            os.remove(gz_path)
            print(f"  del   {fname}  (source removed)")

    # Always recompress — avoids serving stale .gz on uploadfs.
    for fname in sorted(os.listdir(data_dir)):
        if fname.endswith(".gz"):
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

env.AddPreAction("uploadfs", gzip_data_files)
