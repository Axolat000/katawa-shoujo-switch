"""Copies game assets into romfs/game, writes romfs/data/images.tsv, makes the app icon and transcodes
videos to MPEG-1."""
import io, os, shutil, struct, subprocess, sys, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC_DIRS = [os.path.join(ROOT, "extracted"), os.path.join(ROOT, "extracted_lang"), os.path.join(ROOT, "extracted_r18")]
OUT = os.path.join(ROOT, "romfs", "game")
DATA = os.path.join(ROOT, "romfs", "data")
FFMPEG = os.path.join(ROOT, "..", "tools_ext", "ffmpeg-8.0-essentials_build", "bin", "ffmpeg.exe")

SKIP_EXT = (".rpyc", ".rpy", ".lang")

# App icon (.nro and .nsp): Katawa Shoujo key art from SteamGridDB, converted to a 256x256 baseline JPEG
ICON_URL = "https://cdn2.steamgriddb.com/icon/edcc363aeda7f5a49777a436f6029b00/32/512x512.png"
ICON = os.path.join(ROOT, "icon.jpg")


def image_size(path):
    with open(path, "rb") as f:
        head = f.read(32)
        if head[:8] == b"\x89PNG\r\n\x1a\n":
            w, h = struct.unpack(">II", head[16:24])
            return w, h
        if head[:2] == b"\xff\xd8":
            f.seek(2)
            while True:
                marker = f.read(2)
                if len(marker) < 2 or marker[0] != 0xFF:
                    return None
                code = marker[1]
                if code in (0xD8, 0x01) or 0xD0 <= code <= 0xD7:
                    continue
                seglen = struct.unpack(">H", f.read(2))[0]
                if code in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
                    f.read(1)
                    h, w = struct.unpack(">HH", f.read(4))
                    return w, h
                f.seek(seglen - 2, 1)
    return None


def find_ffmpeg():
    base = os.path.join(ROOT, "..", "tools_ext")
    for d in (os.listdir(base) if os.path.isdir(base) else []):
        cand = os.path.join(base, d, "bin", "ffmpeg.exe")
        if os.path.exists(cand):
            return cand
    return shutil.which("ffmpeg")


def make_icon():
    if os.path.exists(ICON):
        return
    from PIL import Image
    req = urllib.request.Request(ICON_URL, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req) as r:
        im = Image.open(io.BytesIO(r.read())).convert("RGB")
    im.resize((256, 256), Image.LANCZOS).save(ICON, "JPEG", quality=92, progressive=False)
    print("icon: %s" % ICON)


def main():
    videos = "--videos" in sys.argv
    make_icon()
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(DATA, exist_ok=True)
    sizes = []
    copied = 0
    for src in SRC_DIRS:
        for dirpath, _dirs, files in os.walk(src):
            for fn in files:
                if fn.endswith(SKIP_EXT):
                    continue
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, src).replace("\\", "/")
                if rel.startswith("video/") and fn.endswith(".mkv"):
                    continue
                dst = os.path.join(OUT, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if not os.path.exists(dst) or os.path.getsize(dst) != os.path.getsize(full):
                    shutil.copyfile(full, dst)
                    copied += 1
                if fn.lower().endswith((".png", ".jpg", ".jpeg")):
                    sz = image_size(full)
                    if sz:
                        sizes.append((rel, sz[0], sz[1]))
    with open(os.path.join(DATA, "images.tsv"), "w", encoding="utf-8", newline="\n") as f:
        for rel, w, h in sorted(sizes):
            f.write("%s\t%d\t%d\n" % (rel, w, h))
    print("copied %d files, %d image sizes" % (copied, len(sizes)))

    if videos:
        ff = find_ffmpeg()
        if not ff:
            print("ffmpeg not found")
            return
        vsrc = os.path.join(ROOT, "extracted", "video")
        for fn in sorted(os.listdir(vsrc)):
            if not fn.endswith(".mkv"):
                continue
            dst = os.path.join(OUT, "video", fn[:-4] + ".mpg")
            if os.path.exists(dst):
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            cmd = [ff, "-y", "-loglevel", "error", "-i", os.path.join(vsrc, fn),
                   "-c:v", "mpeg1video", "-q:v", "3", "-bf", "0", "-r", "30",
                   "-c:a", "mp2", "-b:a", "192k", "-ar", "44100", "-ac", "2",
                   "-f", "mpeg", dst]
            print("transcoding", fn)
            subprocess.check_call(cmd)


if __name__ == "__main__":
    main()
