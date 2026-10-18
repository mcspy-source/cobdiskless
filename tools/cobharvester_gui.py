#!/usr/bin/env python3
"""CobHarvester operator GUI - dark-themed Tkinter wrapper (stdlib only).

Wraps cobharvester.exe: offline VHD / offline root / live modes, BCD safety
flags, live command preview, threaded output streaming, emergency cleanup.

Only standard-library modules are used: tkinter, subprocess, threading, os,
sys, ctypes, glob.
"""
import glob
import os
import subprocess
import sys
import threading

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, scrolledtext
except ImportError:
    sys.stderr.write("tkinter is required (bundled with standard Python).\n")
    sys.exit(2)

try:
    import ctypes
    from ctypes import wintypes
except ImportError:  # non-Windows interpreter: admin features degrade
    ctypes = None

CREATE_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

BG = "#1b1e24"
PANEL = "#23262e"
FG = "#d7dae0"
ACCENT = "#4da3ff"
GREEN = "#3fd97c"
RED = "#ff5d5d"
YELLOW = "#ffcf4d"
GRAY = "#8a8f98"


# --------------------------------------------------------------------------
class ToolTip:
    """Minimal hover tooltip (no external dependency)."""

    def __init__(self, widget, text):
        self.widget = widget
        self.text = text
        self.tip = None
        widget.bind("<Enter>", self._show)
        widget.bind("<Leave>", self._hide)

    def _show(self, _event=None):
        if self.tip is not None:
            return
        x = self.widget.winfo_rootx() + 20
        y = self.widget.winfo_rooty() + 20
        self.tip = tw = tk.Toplevel(self.widget)
        tw.wm_overrideredirect(True)
        tw.wm_geometry("+%d+%d" % (x, y))
        tk.Label(tw, text=self.text, justify="left", bg="#0f1115", fg=YELLOW,
                 relief="solid", borderwidth=1, padx=6, pady=4,
                 wraplength=380).pack()

    def _hide(self, _event=None):
        if self.tip is not None:
            self.tip.destroy()
            self.tip = None


# --------------------------------------------------------------------------
def is_admin():
    if ctypes is None:
        return False
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except OSError:
        return False


def relaunch_as_admin():
    if ctypes is None:
        return False
    try:
        script = os.path.abspath(sys.argv[0])
        params = '\"%s\"' % script
        ctypes.windll.shell32.ShellExecuteW(None, "runas", sys.executable,
                                            params, None, 1)
        return True
    except OSError:
        return False


def find_harvester_exe():
    here = os.path.dirname(os.path.abspath(__file__))
    cands = [
        os.path.join(here, "cobharvester.exe"),
        os.path.join(os.getcwd(), "cobharvester.exe"),
        os.path.join(here, "release", "cobharvester.exe"),
        os.path.join(os.getcwd(), "release", "cobharvester.exe"),
        os.path.join(here, "build", "bin", "cobharvester.exe"),
        os.path.join(os.getcwd(), "build", "bin", "cobharvester.exe"),
        os.path.join(os.path.dirname(here), "cobdiskless", "release",
                     "cobharvester.exe"),
    ]
    for c in cands:
        if os.path.isfile(c):
            return os.path.abspath(c)
    return ""


def scan_vhds():
    roots = {os.path.dirname(os.path.abspath(__file__)), os.getcwd()}
    try:
        roots.add(os.path.dirname(os.getcwd()))
    except OSError:
        pass
    found = []
    for r in roots:
        for pat in ("*.vhd", "*.vhdx"):
            try:
                found.extend(glob.glob(os.path.join(r, pat)))
            except OSError:
                pass
    return sorted(set(os.path.abspath(p) for p in found))


def newest_backup_next_to(vhd_path):
    """Newest <vhd>.{bios,efi}.BCD-backup-*.bak or ''."""
    if not vhd_path:
        return ""
    best, best_mtime = "", -1.0
    for suffix in (".bios.BCD-backup-", ".efi.BCD-backup-"):
        for bak in glob.glob(vhd_path + suffix + "*.bak"):
            try:
                mt = os.path.getmtime(bak)
            except OSError:
                continue
            if mt > best_mtime:
                best, best_mtime = bak, mt
    return best


# --------------------------------------------------------------------------
class HarvesterGUI(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("CobHarvester Operator Console")
        self.configure(bg=BG)
        self.geometry("980x760")

        self.exe_var = tk.StringVar(value=find_harvester_exe())
        self.mode_var = tk.StringVar(value="vhd")
        self.live_ok_var = tk.BooleanVar(value=False)
        self.vhd_var = tk.StringVar()
        self.root_var = tk.StringVar()
        self.restore_on_var = tk.BooleanVar(value=False)
        self.restore_var = tk.StringVar()
        self.skip_bcd_var = tk.BooleanVar(value=False)
        self.unsigned_var = tk.BooleanVar(value=False)
        self.fixdev_var = tk.BooleanVar(value=False)

        self._lines = []
        self._lock = threading.Lock()
        self._proc = None
        self._polling = False

        self._build_header()
        self._build_exe_row()
        self._build_modes()
        self._build_flags()
        self._build_preview()
        self._build_console()
        self._build_actions()
        self._refresh_preview()

    # -- widget helpers ----------------------------------------------------
    def _labelframe(self, parent, text):
        lf = tk.LabelFrame(parent, text=text, bg=PANEL, fg=ACCENT,
                           font=("Segoe UI", 9, "bold"), padx=8, pady=6)
        lf.pack(fill="x", padx=10, pady=5)
        return lf

    def _entry(self, parent, var, width=70):
        return tk.Entry(parent, textvariable=var, width=width, bg="#101218",
                        fg=FG, insertbackground=FG, relief="flat")

    def _btn(self, parent, text, cmd, fg=FG):
        return tk.Button(parent, text=text, command=cmd, bg="#2e3340", fg=fg,
                         activebackground="#3a4152", relief="flat", padx=10)

    # -- layout ------------------------------------------------------------
    def _build_header(self):
        bar = tk.Frame(self, bg=BG)
        bar.pack(fill="x", padx=10, pady=(10, 0))
        tk.Label(bar, text="CobHarvester", bg=BG, fg=FG,
                 font=("Segoe UI", 14, "bold")).pack(side="left")
        self.admin_badge = tk.Label(bar, text="checking...", bg=BG, fg=GRAY,
                                    font=("Segoe UI", 10, "bold"))
        self.admin_badge.pack(side="left", padx=12)
        self._btn(bar, "Relaunch as Admin",
                  self._on_relaunch_admin, fg=GREEN).pack(side="right")
        self._refresh_admin_badge()

    def _refresh_admin_badge(self):
        if is_admin():
            self.admin_badge.config(text="ADMIN", fg=GREEN)
        else:
            self.admin_badge.config(text="NOT ADMIN", fg=RED)

    def _on_relaunch_admin(self):
        if is_admin():
            messagebox.showinfo("Admin", "Already running elevated.")
            return
        if relaunch_as_admin():
            self.destroy()
        else:
            messagebox.showerror("Admin", "Elevation request failed.")

    def _build_exe_row(self):
        lf = self._labelframe(self, "cobharvester.exe")
        row = tk.Frame(lf, bg=PANEL)
        row.pack(fill="x")
        self._entry(row, self.exe_var).pack(side="left", fill="x", expand=True)
        self._btn(row, "Browse...",
                  self._browse_exe).pack(side="left", padx=(8, 0))
        self.exe_var.trace_add("write", lambda *_: self._refresh_preview())

    def _browse_exe(self):
        p = filedialog.askopenfilename(
            title="Locate cobharvester.exe",
            filetypes=[("cobharvester", "cobharvester.exe"),
                       ("Executables", "*.exe")])
        if p:
            self.exe_var.set(p)

    def _build_modes(self):
        lf = self._labelframe(self, "Target mode")
        # VHD row
        r1 = tk.Frame(lf, bg=PANEL)
        r1.pack(fill="x", pady=2)
        tk.Radiobutton(r1, text="Offline VHD  (--vhd)", variable=self.mode_var,
                       value="vhd", bg=PANEL, fg=FG, selectcolor=BG,
                       activebackground=PANEL,
                       command=self._refresh_preview).pack(side="left")
        self._entry(r1, self.vhd_var, width=52).pack(side="left", padx=8)
        self._btn(r1, "Browse...",
                  self._browse_vhd).pack(side="left")
        self.vhd_combo_var = tk.StringVar(value="(auto-scan: no VHD found)")
        self.vhd_combo = tk.OptionMenu(r1, self.vhd_combo_var, ())
        self.vhd_combo.config(bg="#2e3340", fg=FG, activebackground="#3a4152",
                              relief="flat")
        self.vhd_combo.pack(side="left", padx=8)
        self._refresh_vhd_scan()
        # Root row
        r2 = tk.Frame(lf, bg=PANEL)
        r2.pack(fill="x", pady=2)
        tk.Radiobutton(r2, text="Offline Root (--root)", variable=self.mode_var,
                       value="root", bg=PANEL, fg=FG, selectcolor=BG,
                       activebackground=PANEL,
                       command=self._refresh_preview).pack(side="left")
        self._entry(r2, self.root_var, width=52).pack(side="left", padx=8)
        self._btn(r2, "Pick folder...",
                  self._browse_root).pack(side="left")
        tk.Label(r2, text="C: is blocked automatically", bg=PANEL, fg=GRAY,
                 font=("Segoe UI", 8)).pack(side="left", padx=8)
        # Live row (behind red toggle)
        r3 = tk.Frame(lf, bg=PANEL)
        r3.pack(fill="x", pady=2)
        self.live_radio = tk.Radiobutton(
            r3, text="Live Golden Client  (NO flag - touches THIS machine)",
            variable=self.mode_var, value="live", bg=PANEL, fg=RED,
            selectcolor=BG, activebackground=PANEL, state="disabled",
            command=self._refresh_preview)
        self.live_radio.pack(side="left")
        tk.Checkbutton(r3, text="I UNDERSTAND - enable Live mode",
                       variable=self.live_ok_var, bg=PANEL, fg=RED,
                       selectcolor=BG, activebackground=PANEL,
                       command=self._toggle_live).pack(side="left", padx=12)
        for v in (self.vhd_var, self.root_var, self.mode_var):
            v.trace_add("write", lambda *_: self._refresh_preview())

    def _toggle_live(self):
        self.live_radio.config(
            state="normal" if self.live_ok_var.get() else "disabled")
        if not self.live_ok_var.get() and self.mode_var.get() == "live":
            self.mode_var.set("vhd")
        self._refresh_preview()

    def _refresh_vhd_scan(self):
        found = scan_vhds()
        menu = self.vhd_combo["menu"]
        menu.delete(0, "end")
        if not found:
            self.vhd_combo_var.set("(auto-scan: no VHD found)")
            return
        self.vhd_combo_var.set("pick scanned VHD (%d)" % len(found))
        for path in found:
            menu.add_command(label=path, command=lambda p=path: self.vhd_var.set(p))
        if not self.vhd_var.get():
            self.vhd_var.set(found[0])

    def _browse_vhd(self):
        p = filedialog.askopenfilename(
            title="Select VHD/VHDX",
            filetypes=[("Virtual disks", "*.vhd *.vhdx"),
                       ("All files", "*.*")])
        if p:
            self.vhd_var.set(p)

    def _browse_root(self):
        system_drive = os.environ.get("SystemDrive", "C:").upper()
        p = filedialog.askdirectory(title="Select mounted Windows partition")
        if not p:
            return
        drive = os.path.splitdrive(os.path.abspath(p))[0].upper()
        if drive == system_drive or drive == "C:":
            messagebox.showerror("Blocked",
                                 "Refusing %s: that is the system drive." % p)
            return
        self.root_var.set(p)

    def _build_flags(self):
        lf = self._labelframe(self, "BCD & advanced flags")
        # restore row
        r = tk.Frame(lf, bg=PANEL)
        r.pack(fill="x", pady=2)
        tk.Checkbutton(r, text="Restore BCD backup first  (--restore-bcd)",
                       variable=self.restore_on_var, bg=PANEL, fg=FG,
                       selectcolor=BG, activebackground=PANEL,
                       command=self._refresh_preview).pack(side="left")
        self._entry(r, self.restore_var, width=44).pack(side="left", padx=8)
        self._btn(r, "Browse *.bak",
                  self._browse_restore).pack(side="left")
        self._btn(r, "Auto",
                  self._auto_restore).pack(side="left", padx=(6, 0))
        # skip row
        r2 = tk.Frame(lf, bg=PANEL)
        r2.pack(fill="x", pady=2)
        cb1 = tk.Checkbutton(r2, text="Skip BCD edits  (--skip-bcd)",
                             variable=self.skip_bcd_var, bg=PANEL, fg=FG,
                             selectcolor=BG, activebackground=PANEL,
                             command=self._refresh_preview)
        cb1.pack(side="left")
        ToolTip(cb1, "Drivers + registry only. BCD (and --restore-bcd) "
                     "left completely untouched.")
        cb2 = tk.Checkbutton(r2, text="Unsigned drivers / TestSigning  (--unsigned-drivers)",
                             variable=self.unsigned_var, bg=PANEL, fg=YELLOW,
                             selectcolor=BG, activebackground=PANEL,
                             command=self._refresh_preview)
        cb2.pack(side="left", padx=16)
        ToolTip(cb2, "WARNING: breaks UEFI Secure Boot (0xc0000428) and trips "
                     "kernel anti-cheats (Vanguard / EAC / FaceIt). Only for "
                     "genuinely unsigned driver payloads.")
        cb3 = tk.Checkbutton(r2, text="Force BCD device rewrite  (--fix-bcd-device)",
                             variable=self.fixdev_var, bg=PANEL, fg=RED,
                             selectcolor=BG, activebackground=PANEL,
                             command=self._refresh_preview)
        cb3.pack(side="left", padx=16)
        ToolTip(cb3, "DANGEROUS: rewrites {default} device/osdevice locators. "
                     "This is what caused 0xc000000e. Offline only, broken "
                     "locators only.")

    def _browse_restore(self):
        p = filedialog.askopenfilename(
            title="Select BCD backup",
            filetypes=[("BCD backups", "*.bak"), ("All files", "*.*")])
        if p:
            self.restore_on_var.set(True)
            self.restore_var.set(p)
            self._refresh_preview()

    def _auto_restore(self):
        vhd = self.vhd_var.get().strip()
        bak = newest_backup_next_to(vhd)
        if bak:
            self.restore_on_var.set(True)
            self.restore_var.set(bak)
            self._refresh_preview()
        else:
            # Fall back to CLI-side auto-detection.
            self.restore_on_var.set(True)
            self.restore_var.set("auto")
            self._refresh_preview()

    # -- command line ------------------------------------------------------
    def build_argv(self):
        exe = self.exe_var.get().strip()
        argv = [exe if exe else "cobharvester.exe"]
        mode = self.mode_var.get()
        if mode == "vhd":
            argv += ["--vhd", self.vhd_var.get().strip()]
        elif mode == "root":
            argv += ["--root", self.root_var.get().strip()]
        elif mode == "live":
            argv += ["--live"]
        if self.restore_on_var.get() and self.restore_var.get().strip():
            argv += ["--restore-bcd", self.restore_var.get().strip()]
        if self.skip_bcd_var.get():
            argv += ["--skip-bcd"]
        if self.unsigned_var.get():
            argv += ["--unsigned-drivers"]
        if self.fixdev_var.get():
            argv += ["--fix-bcd-device"]
        return argv

    def _refresh_preview(self, *_args):
        try:
            shown = " ".join('"%s"' % a if " " in a else a
                              for a in self.build_argv())
            self.preview.config(state="normal")
            self.preview.delete("1.0", "end")
            self.preview.insert("1.0", shown)
            self.preview.config(state="disabled")
        except (AttributeError, tk.TclError):
            pass

    def _build_preview(self):
        lf = self._labelframe(self, "Command preview (exactly what runs)")
        self.preview = tk.Text(lf, height=2, bg="#101218", fg=ACCENT,
                               relief="flat", wrap="word")
        self.preview.pack(fill="x")
        self.preview.config(state="disabled")

    # -- console + execution -----------------------------------------------
    def _build_console(self):
        lf = self._labelframe(self, "Execution stream")
        self.console = scrolledtext.ScrolledText(
            lf, height=16, bg="#0d0f13", fg=FG, relief="flat",
            font=("Consolas", 9))
        self.console.pack(fill="both", expand=True)
        for tag, color in (("ok", GREEN), ("warn", YELLOW), ("err", RED),
                           ("dim", GRAY), ("info", ACCENT)):
            self.console.tag_config(tag, foreground=color)
        self.console.config(state="disabled")

    def _append(self, text, tag="info"):
        self.console.config(state="normal")
        self.console.insert("end", text, tag)
        self.console.see("end")
        self.console.config(state="disabled")

    @staticmethod
    def _tag_for(line):
        s = line.strip()
        if s.startswith("[FAIL]") or "FAILED" in s or "error" in s.lower():
            return "err"
        if "[!]" in s or "WARNING" in s:
            return "warn"
        if s.startswith("[OK]") or "[+]" in s:
            return "ok"
        if s.startswith("[--]"):
            return "dim"
        return "info"

    def _drain(self):
        with self._lock:
            pending, self._lines = self._lines, []
        for line in pending:
            self._append(line, self._tag_for(line))
        if self._proc is not None and self._proc.poll() is None:
            self.after(120, self._drain)
            return
        # Process ended: flush remainder, report exit code.
        rc = self._proc.poll() if self._proc is not None else -1
        self._proc = None
        self._polling = False
        tag = "ok" if rc == 0 else "err"
        self._append("\n[exit=%s]\n" % rc, tag)
        self.run_btn.config(state="normal")
        self.stop_btn.config(state="disabled")

    def _validate(self):
        exe = self.exe_var.get().strip()
        if not exe or not os.path.isfile(exe):
            return "cobharvester.exe not found - set the path above."
        mode = self.mode_var.get()
        if mode == "vhd":
            v = self.vhd_var.get().strip()
            if not v:
                return "Pick a VHD file first."
            if not os.path.isfile(v):
                return "VHD not found: %s" % v
            if not v.lower().endswith((".vhd", ".vhdx")):
                return "Not a .vhd/.vhdx file: %s" % v
        elif mode == "root":
            r = self.root_var.get().strip()
            if not r or not os.path.isdir(r):
                return "Pick a mounted Windows partition folder first."
            system_drive = os.environ.get("SystemDrive", "C:").upper()
            drive = os.path.splitdrive(os.path.abspath(r))[0].upper()
            if drive == system_drive:
                return "Refusing the system drive."
            hive = os.path.join(r, "Windows", "System32", "config", "SYSTEM")
            if not os.path.isfile(hive):
                return ("No SYSTEM hive at %s - pick the partition "
                        "containing \\Windows." % hive)
        if mode == "live" and self.restore_on_var.get():
            return ("--restore-bcd is refused in Live mode by the harvester; "
                    "untick it or switch to an offline mode.")
        if mode == "live" and self.fixdev_var.get():
            return ("--fix-bcd-device is offline-only; untick it for Live "
                    "mode.")
        return ""

    def _build_actions(self):
        bar = tk.Frame(self, bg=BG)
        bar.pack(fill="x", padx=10, pady=8)
        self.run_btn = self._btn(bar, "RUN harvester", self._on_run, fg=GREEN)
        self.run_btn.pack(side="left")
        self.stop_btn = self._btn(bar, "Stop", self._on_stop, fg=RED)
        self.stop_btn.pack(side="left", padx=8)
        self.stop_btn.config(state="disabled")
        self._btn(bar, "Emergency detach / unload cleanup",
                  self._on_cleanup, fg=YELLOW).pack(side="right")

    def _on_run(self):
        err = self._validate()
        if err:
            messagebox.showerror("Cannot run", err)
            return
        if not is_admin():
            messagebox.showwarning(
                "Not elevated",
                "This GUI is NOT Admin: the harvester will re-spawn elevated "
                "and its detailed output will appear in the new window, not "
                "here. Use 'Relaunch as Admin' for full streaming.")
        argv = self.build_argv()
        self._append("$ " + " ".join(argv) + "\n", "info")
        threading.Thread(target=self._run_thread, args=(argv,),
                         daemon=True).start()
        self.run_btn.config(state="disabled")
        self.stop_btn.config(state="normal")
        if not self._polling:
            self._polling = True
            self.after(120, self._drain)

    def _run_thread(self, argv):
        try:
            self._proc = subprocess.Popen(
                argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, bufsize=1, creationflags=CREATE_NO_WINDOW)
        except OSError as exc:
            with self._lock:
                self._lines.append("[FAIL] launch failed: %s\n" % exc)
            self._proc = None
            return
        try:
            for line in self._proc.stdout:
                with self._lock:
                    self._lines.append(line)
        except (OSError, ValueError):
            pass

    def _on_stop(self):
        proc, self._proc = self._proc, self._proc
        if proc is not None and proc.poll() is None:
            try:
                proc.terminate()
            except OSError:
                pass

    # -- emergency cleanup ---------------------------------------------------
    def _on_cleanup(self):
        vhd = self.vhd_var.get().strip() if self.mode_var.get() == "vhd" else ""
        threading.Thread(target=self._cleanup_thread, args=(vhd,),
                         daemon=True).start()
        if not self._polling:
            self._polling = True
            self._proc = None
            self.after(120, self._cleanup_drain)

    def _cleanup_drain(self):
        with self._lock:
            pending, self._lines = self._lines, []
        for line in pending:
            self._append(line, self._tag_for(line))
        self._polling = False

    def _cleanup_thread(self, vhd):
        def emit(line, _tag="warn"):
            with self._lock:
                self._lines.append(line)

        emit("[!] Emergency cleanup: unloading stale hives...\n")
        for key in (r"HKLM\COB_OFFLINE_SYS", r"HKLM\VHD_SYS", r"HKLM\VHD_SFT"):
            try:
                p = subprocess.run(
                    ["reg", "unload", key], capture_output=True, text=True,
                    creationflags=CREATE_NO_WINDOW)
                emit("  reg unload %s -> exit=%d %s\n"
                     % (key, p.returncode, (p.stdout + p.stderr).strip()))
            except OSError as exc:
                emit("  reg unload %s failed: %s\n" % (key, exc))
        if vhd:
            emit("[!] Detaching %s ...\n" % vhd)
            script = ('select vdisk file="%s"\ndetach vdisk\n' % vhd)
            tmp = os.path.join(os.environ.get("TEMP", "."), "cob_emg_detach.txt")
            try:
                with open(tmp, "w", encoding="ascii") as fh:
                    fh.write(script)
                p = subprocess.run(
                    ["diskpart", "/s", tmp], capture_output=True, text=True,
                    creationflags=CREATE_NO_WINDOW)
                emit("  diskpart detach -> exit=%d\n%s\n"
                     % (p.returncode, (p.stdout + p.stderr).strip()))
            except OSError as exc:
                emit("  diskpart failed: %s\n" % exc)
            finally:
                try:
                    os.remove(tmp)
                except OSError:
                    pass
        else:
            emit("  (no VHD selected - hive unload only)\n")
        emit("[!] Cleanup done. Re-attach state is now explicit.\n")


def main():
    app = HarvesterGUI()
    app.mainloop()


if __name__ == "__main__":
    main()
