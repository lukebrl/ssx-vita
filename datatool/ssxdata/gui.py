"""
The window: pick the APK (and the output folder, the tool's own by default),
build, follow the progress.

The build runs on a worker thread; it talks to the window only through a
queue that the Tk loop polls, since Tk isn't thread-safe.
"""

import os
import queue
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from . import __version__, apk, pipeline

POLL_MS = 100


def tool_folder():
    """The folder the tool is in: the executable's (the .app's on macOS), or the datatool folder when run from source."""
    if getattr(sys, "frozen", False):
        here = os.path.dirname(os.path.abspath(sys.executable))
        # macOS: <folder>/ssx-datatool.app/Contents/MacOS/ssx-datatool
        bundle = os.path.dirname(os.path.dirname(here))
        if os.path.basename(here) == "MacOS" and bundle.endswith(".app"):
            here = os.path.dirname(bundle)
        return here
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def default_output():
    """Where the ssx folder goes unless the user picks another: next to the tool, else the desktop."""
    try:
        here = tool_folder()
        # Not writable, for example a macOS app run from where Gatekeeper moved it
        if os.path.isdir(here) and os.access(here, os.W_OK):
            return here
    except (OSError, ValueError):
        pass
    home = os.path.expanduser("~")
    for desktop in (os.path.join(home, "Desktop"), os.path.join(home, "OneDrive", "Desktop")):
        if os.path.isdir(desktop):
            return desktop
    return home


class App:
    def __init__(self, root):
        self.root = root
        self.events = queue.Queue()
        self.worker = None
        root.title(f"SSX Vita data tool {__version__}")
        root.minsize(640, 420)

        frame = ttk.Frame(root, padding=12)
        frame.pack(fill="both", expand=True)
        frame.columnconfigure(1, weight=1)
        frame.rowconfigure(4, weight=1)

        intro = (f"Builds the data the SSX Vita port needs from your own copy of SSX for Android "
                 f"(version {apk.GAME_VERSION}). Then copy the ssx folder it makes to ux0:data/ on your Vita.")
        ttk.Label(frame, text=intro, wraplength=600, justify="left").grid(row=0, column=0, columnspan=3,
                                                                           sticky="w", pady=(0, 10))

        self.apk_path = tk.StringVar()
        self.out_path = tk.StringVar(value=default_output())
        ttk.Label(frame, text="SSX APK:").grid(row=1, column=0, sticky="w")
        ttk.Entry(frame, textvariable=self.apk_path).grid(row=1, column=1, sticky="ew", padx=6)
        self.apk_button = ttk.Button(frame, text="Browse...", command=self.pick_apk)
        self.apk_button.grid(row=1, column=2)
        ttk.Label(frame, text="Output folder:").grid(row=2, column=0, sticky="w", pady=6)
        ttk.Entry(frame, textvariable=self.out_path).grid(row=2, column=1, sticky="ew", padx=6, pady=6)
        self.out_button = ttk.Button(frame, text="Browse...", command=self.pick_output)
        self.out_button.grid(row=2, column=2, pady=6)

        self.progress = ttk.Progressbar(frame, maximum=1000)
        self.progress.grid(row=3, column=0, columnspan=2, sticky="ew", pady=6)
        self.start_button = ttk.Button(frame, text="Build", command=self.start)
        self.start_button.grid(row=3, column=2, pady=6)

        self.log_box = tk.Text(frame, height=14, state="disabled", wrap="word")
        self.log_box.grid(row=4, column=0, columnspan=3, sticky="nsew")
        scroll = ttk.Scrollbar(frame, command=self.log_box.yview)
        scroll.grid(row=4, column=3, sticky="ns")
        self.log_box["yscrollcommand"] = scroll.set

        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(POLL_MS, self.poll)

    def pick_apk(self):
        path = filedialog.askopenfilename(title="SSX APK", filetypes=[("Android package", "*.apk"), ("All", "*")])
        if path:
            self.apk_path.set(path)

    def pick_output(self):
        path = filedialog.askdirectory(title="Output folder", initialdir=self.out_path.get() or None)
        if path:
            self.out_path.set(path)

    def write(self, text):
        self.log_box["state"] = "normal"
        self.log_box.insert("end", text + "\n")
        self.log_box.see("end")
        self.log_box["state"] = "disabled"

    def set_busy(self, busy):
        state = "disabled" if busy else "normal"
        for w in (self.apk_button, self.out_button, self.start_button):
            w["state"] = state

    def start(self):
        apk_path, out = self.apk_path.get().strip(), self.out_path.get().strip()
        if not os.path.isfile(apk_path):
            messagebox.showerror("SSX Vita data tool", "Pick the SSX APK first.")
            return
        if not os.path.isdir(out):
            messagebox.showerror("SSX Vita data tool", "Pick an existing output folder.")
            return
        data = pipeline.data_dir(out)
        overwrite = False
        if os.path.exists(data):
            if not messagebox.askyesno("SSX Vita data tool", f"{data} already exists. Replace it?"):
                return
            overwrite = True
        self.set_busy(True)
        self.progress["value"] = 0
        self.worker = threading.Thread(target=self.run, args=(apk_path, out, overwrite), daemon=True)
        self.worker.start()

    def run(self, apk_path, out, overwrite):
        try:
            pipeline.build(apk_path, out, lambda t: self.events.put(("log", t)),
                           lambda f: self.events.put(("progress", f)), overwrite)
            self.events.put(("done", None))
        except (apk.ApkError, RuntimeError, OSError) as e:
            self.events.put(("error", str(e)))
        except Exception as e:  # shown, not lost in a thread
            self.events.put(("error", f"unexpected error: {e!r}"))

    def poll(self):
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "log":
                    self.write(value)
                elif kind == "progress":
                    self.progress["value"] = value * 1000
                elif kind == "done":
                    self.set_busy(False)
                    messagebox.showinfo("SSX Vita data tool", "Done. Copy the ssx folder to ux0:data/ on your Vita.")
                elif kind == "error":
                    self.set_busy(False)
                    self.write("Error: " + value)
                    messagebox.showerror("SSX Vita data tool", value)
        except queue.Empty:
            pass
        self.root.after(POLL_MS, self.poll)

    def close(self):
        if self.worker and self.worker.is_alive():
            if not messagebox.askyesno("SSX Vita data tool", "A build is running. Quit anyway?"):
                return
        self.root.destroy()


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()
    return 0
