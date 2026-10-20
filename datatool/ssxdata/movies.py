"""
Movies re-encoded for the Vita's hardware decoder.

The APK's movies (assets/common/mp4, ~800 MB) are MPEG-4 Part 2 (ASP) at
1920x1080, which sceAvPlayer can't decode. Each is re-encoded in place with
ffmpeg to H.264 at the screen's 960x544, with 48 kHz stereo AAC (the rate
of the audio port the port plays them on) and the index at the start of the
file, where the port checks that a movie is H.264 before playing it
(source/jni/jni_video.c). About 135 MB for the 7.5 minutes of movies.

ffmpeg comes from, in order: the SSX_FFMPEG environment variable, the
imageio-ffmpeg package (bundled in the packaged tool), or the PATH. Without
it the movies are removed, and the game skips them.
"""

import os
import shutil
import subprocess

MOVIE_DIR = os.path.join("common", "mp4")
WIDTH, HEIGHT = 960, 544
VIDEO = ["-c:v", "libx264", "-preset", "slow", "-profile:v", "main", "-level", "3.1", "-pix_fmt", "yuv420p",
         "-crf", "21", "-maxrate", "4M", "-bufsize", "8M"]
AUDIO = ["-c:a", "aac", "-b:a", "128k", "-ar", "48000", "-ac", "2"]


def find_ffmpeg():
    path = os.environ.get("SSX_FFMPEG")
    if path and os.path.isfile(path):
        return path
    try:
        import imageio_ffmpeg

        return imageio_ffmpeg.get_ffmpeg_exe()
    except Exception:
        pass
    return shutil.which("ffmpeg")


def command(ffmpeg, src, dst):
    return [ffmpeg, "-v", "error", "-y", "-i", src, "-vf", f"scale={WIDTH}:{HEIGHT}:flags=lanczos", *VIDEO, *AUDIO,
            "-movflags", "+faststart", dst]


def _run(cmd):
    # No console window for each ffmpeg when the tool runs as a window on Windows
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    r = subprocess.run(cmd, capture_output=True, text=True, creationflags=flags)
    if r.returncode != 0:
        raise RuntimeError(f"ffmpeg failed on {os.path.basename(cmd[cmd.index('-i') + 1])}: {r.stderr.strip()[-500:]}")


def run(assets, log, progress):
    folder = os.path.join(assets, MOVIE_DIR)
    names = sorted(n for n in os.listdir(folder) if n.lower().endswith(".mp4")) if os.path.isdir(folder) else []
    if not names:
        log("  no movies found")
        return
    ffmpeg = find_ffmpeg()
    if not ffmpeg:
        for n in names:
            os.remove(os.path.join(folder, n))
        log("  ffmpeg not found: the movies were removed and the game will skip them "
            "(install ffmpeg, or set SSX_FFMPEG, and build again to get them)")
        return
    sizes = {n: os.path.getsize(os.path.join(folder, n)) for n in names}
    total, done, before, after = sum(sizes.values()), 0, 0, 0
    for n in names:
        src = os.path.join(folder, n)
        tmp = src + ".tmp.mp4"
        _run(command(ffmpeg, src, tmp))
        before += sizes[n]
        after += os.path.getsize(tmp)
        os.replace(tmp, src)
        done += sizes[n]
        progress(done / total)
    log(f"  {len(names)} movies: {before / 2**20:.0f} MB -> {after / 2**20:.0f} MB")
