"""M5Grader config.json GUI Editor"""
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import json
import re
from pathlib import Path

# ---- Constants ----
SECTION_BG = "#f5f5f5"

PALETTE_COLORS = [
    # Row 1: Red → Yellow-green
    "#FF0000","#FF4500","#FF8C00","#FFA500","#FFD700","#FFFF00","#ADFF2F","#00FF00",
    # Row 2: Green → Magenta
    "#00FF7F","#00FFFF","#00BFFF","#0080FF","#0000FF","#4B0082","#8B00FF","#FF00FF",
    # Row 3: Pink → Dark
    "#FF69B4","#FF1493","#DC143C","#800000","#808000","#006400","#008080","#000080",
    # Row 4: Grayscale
    "#FFFFFF","#D0D0D0","#A0A0A0","#808080","#606060","#404040","#202020","#000000",
]


def contrast_color(hex_color):
    """Return #000000 or #FFFFFF for readable text on the given background."""
    h = hex_color.lstrip("#")
    r, g, b = int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)
    return "#000000" if (0.299 * r + 0.587 * g + 0.114 * b) >= 128 else "#FFFFFF"


def is_valid_color(s):
    return bool(re.match(r"^#[0-9A-Fa-f]{6}$", s))


# ============================================================
class GradeDialog(tk.Toplevel):
    """Add / Edit grade dialog."""

    def __init__(self, parent, title, grade=None):
        super().__init__(parent)
        self.title(title)
        self.resizable(False, False)
        self.result = None
        self._selected_color = (grade or {}).get("color", "#FF0000").upper()
        self._build(grade or {})
        self.transient(parent)
        self.grab_set()
        x = parent.winfo_rootx() + 80
        y = parent.winfo_rooty() + 40
        self.geometry(f"+{x}+{y}")
        self.wait_window()

    # ---- Build ----

    def _build(self, grade):
        body = tk.Frame(self, bg="white", padx=14, pady=10)
        body.pack(fill=tk.BOTH, expand=True)

        # Basic fields
        self._vars = {}
        defs = [
            ("規格名:",        "name",       grade.get("name", ""),                 None),
            ("最小重量 (g):",  "min_weight", str(grade.get("min_weight", "")),      None),
            ("最大重量 (g):",  "max_weight", str(grade.get("max_weight", "")),      None),
            ("音声ファイル:",  "sound_file", grade.get("sound_file", ""),           self._browse_wav),
        ]
        for label, key, value, browse_cmd in defs:
            fr = tk.Frame(body, bg="white")
            fr.pack(fill=tk.X, pady=3)
            tk.Label(fr, text=label, width=14, anchor="e", bg="white").pack(side=tk.LEFT)
            var = tk.StringVar(value=value)
            self._vars[key] = var
            tk.Entry(fr, textvariable=var, width=24).pack(side=tk.LEFT, padx=(4, 0))
            if browse_cmd:
                tk.Button(fr, text="参照...", command=browse_cmd, padx=4).pack(side=tk.LEFT, padx=4)

        ttk.Separator(body, orient="horizontal").pack(fill=tk.X, pady=8)

        # Color palette
        tk.Label(body, text="背景色:", anchor="w", bg="white",
                 font=("", 9, "bold")).pack(anchor="w")

        grid_fr = tk.Frame(body, bg="white")
        grid_fr.pack(anchor="w", pady=(4, 0))
        self._swatch_widgets = []
        for i, color in enumerate(PALETTE_COLORS):
            lbl = tk.Label(grid_fr, bg=color, width=2, height=1,
                           relief="solid", bd=1, cursor="hand2")
            lbl.grid(row=i // 8, column=i % 8, padx=2, pady=2)
            lbl.bind("<Button-1>", lambda e, c=color: self._pick(c))
            self._swatch_widgets.append((color, lbl))

        # HEX input + preview bar
        prev_fr = tk.Frame(body, bg="white")
        prev_fr.pack(fill=tk.X, pady=(8, 0))
        tk.Label(prev_fr, text="HEX:", bg="white").pack(side=tk.LEFT)
        self._hex_var = tk.StringVar(value=self._selected_color)
        hex_entry = tk.Entry(prev_fr, textvariable=self._hex_var, width=10,
                             font=("Consolas", 11))
        hex_entry.pack(side=tk.LEFT, padx=4)
        hex_entry.bind("<FocusOut>", self._on_hex_change)
        hex_entry.bind("<Return>",   self._on_hex_change)
        self._preview = tk.Label(prev_fr, text="", bg=self._selected_color,
                                 relief="solid", bd=1)
        self._preview.pack(side=tk.LEFT, fill=tk.X, expand=True)

        self._update_palette_highlight()

        # Footer
        footer = tk.Frame(self, bg="#f0f0f0", padx=10, pady=6)
        footer.pack(fill=tk.X)
        ttk.Separator(footer).pack(fill=tk.X, pady=(0, 6))
        btn_fr = tk.Frame(footer, bg="#f0f0f0")
        btn_fr.pack(anchor="e")
        tk.Button(btn_fr, text="OK",      width=8,  command=self._ok).pack(side=tk.LEFT, padx=4)
        tk.Button(btn_fr, text="キャンセル", width=10, command=self.destroy).pack(side=tk.LEFT)

    # ---- Handlers ----

    def _browse_wav(self):
        path = filedialog.askopenfilename(
            filetypes=[("WAV files", "*.wav"), ("All files", "*.*")],
            title="音声ファイルを選択", parent=self)
        if path:
            self._vars["sound_file"].set("/" + Path(path).name)

    def _pick(self, color):
        self._selected_color = color.upper()
        self._hex_var.set(self._selected_color)
        self._preview.config(bg=self._selected_color)
        self._update_palette_highlight()

    def _on_hex_change(self, _event=None):
        val = self._hex_var.get().strip().upper()
        if not val.startswith("#"):
            val = "#" + val
        if is_valid_color(val):
            self._selected_color = val
            self._hex_var.set(val)
            self._preview.config(bg=val)
            self._update_palette_highlight()

    def _update_palette_highlight(self):
        for color, lbl in self._swatch_widgets:
            if color.upper() == self._selected_color:
                lbl.config(bd=3, relief="solid",
                           highlightbackground="black", highlightthickness=2)
            else:
                lbl.config(bd=1, relief="solid")

    def _ok(self):
        name   = self._vars["name"].get().strip()
        sound  = self._vars["sound_file"].get().strip()
        min_s  = self._vars["min_weight"].get().strip()
        max_s  = self._vars["max_weight"].get().strip()
        color  = self._hex_var.get().strip().upper()

        errors = []
        if not name:
            errors.append("規格名を入力してください。")
        if not sound:
            errors.append("音声ファイルを入力してください。")
        try:
            min_w = int(min_s)
            if min_w < 0:
                raise ValueError
        except ValueError:
            errors.append("最小重量は 0 以上の整数で入力してください。")
            min_w = None
        try:
            max_w = int(max_s)
        except ValueError:
            errors.append("最大重量は整数で入力してください。")
            max_w = None
        if min_w is not None and max_w is not None and min_w >= max_w:
            errors.append("最大重量は最小重量より大きくしてください。")
        if not is_valid_color(color):
            errors.append("背景色は #RRGGBB 形式で入力してください。")

        if errors:
            messagebox.showerror("入力エラー", "\n".join(errors), parent=self)
            return

        self.result = {
            "name": name,
            "min_weight": min_w,
            "max_weight": max_w,
            "sound_file": sound,
            "color": color,
        }
        self.destroy()


# ============================================================
class ConfigEditor(tk.Tk):

    def __init__(self):
        super().__init__()
        self.title("M5Grader Config Editor")
        self.geometry("820x720")
        self.minsize(700, 500)
        self._filepath = None
        self._data = None
        self._modified = False
        self._grades = []
        self._color_canvases = []
        self._build_ui()
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    # ================================================================
    #  UI Construction
    # ================================================================

    def _build_ui(self):
        self._build_filebar()
        self._build_scroll_area()
        self._build_network_section()
        self._build_device_section()
        self._build_product_section()
        self._build_grades_section()
        self._build_statusbar()

    def _build_filebar(self):
        bar = tk.Frame(self, bg="#e8e8e8")
        bar.pack(fill=tk.X, side=tk.TOP)
        inner = tk.Frame(bar, bg="#e8e8e8")
        inner.pack(fill=tk.X, padx=6, pady=5)

        tk.Button(inner, text="📂 開く", command=self._open_file,
                  padx=6).pack(side=tk.LEFT)

        self._path_var = tk.StringVar(value="（ファイルが選択されていません）")
        tk.Entry(inner, textvariable=self._path_var, state="readonly",
                 fg="#555", readonlybackground="white"
                 ).pack(side=tk.LEFT, fill=tk.X, expand=True, padx=6)

        tk.Button(inner, text="💾 保存",
                  bg="#2a6bc5", fg="white",
                  activebackground="#1a5ab5", activeforeground="white",
                  command=self._save, padx=6).pack(side=tk.LEFT)
        tk.Button(inner, text="名前を付けて保存...",
                  command=self._save_as, padx=6).pack(side=tk.LEFT, padx=(4, 0))

        ttk.Separator(bar, orient="horizontal").pack(fill=tk.X)

    def _build_scroll_area(self):
        outer = tk.Frame(self)
        outer.pack(fill=tk.BOTH, expand=True)

        self._canvas = tk.Canvas(outer, borderwidth=0, highlightthickness=0,
                                 bg="#e8e8e8")
        vsb = ttk.Scrollbar(outer, orient="vertical", command=self._canvas.yview)
        self._scroll_frame = tk.Frame(self._canvas, bg="#e8e8e8")

        self._scroll_frame.bind(
            "<Configure>",
            lambda e: self._canvas.configure(
                scrollregion=self._canvas.bbox("all")))

        self._canvas.create_window((0, 0), window=self._scroll_frame, anchor="nw")
        self._canvas.configure(yscrollcommand=vsb.set)

        vsb.pack(side=tk.RIGHT, fill=tk.Y)
        self._canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        self._canvas.bind("<MouseWheel>",
            lambda e: self._canvas.yview_scroll(-1 * (e.delta // 120), "units"))
        self._scroll_frame.bind("<MouseWheel>",
            lambda e: self._canvas.yview_scroll(-1 * (e.delta // 120), "units"))

    def _make_section(self, title):
        sec = tk.LabelFrame(self._scroll_frame, text=f"  {title}  ",
                            font=("", 9, "bold"), fg="#1a3a5a",
                            bg=SECTION_BG, padx=10, pady=6)
        sec.pack(fill=tk.X, padx=8, pady=4)
        return sec

    def _form_row(self, parent, label_text, width=28):
        """Create a label+entry row. Returns (StringVar, row_frame)."""
        fr = tk.Frame(parent, bg=SECTION_BG)
        fr.pack(fill=tk.X, pady=2)
        tk.Label(fr, text=label_text, width=16, anchor="e",
                 bg=SECTION_BG).pack(side=tk.LEFT)
        var = tk.StringVar()
        tk.Entry(fr, textvariable=var, width=width).pack(side=tk.LEFT, padx=(4, 0))
        var.trace_add("write", self._mark_modified)
        return var, fr

    def _build_network_section(self):
        sec = self._make_section("🌐 ネットワーク設定")

        self._ssid_var, _ = self._form_row(sec, "WiFi SSID:")

        # Password row (special: show/hide button)
        pw_fr = tk.Frame(sec, bg=SECTION_BG)
        pw_fr.pack(fill=tk.X, pady=2)
        tk.Label(pw_fr, text="WiFi パスワード:", width=16, anchor="e",
                 bg=SECTION_BG).pack(side=tk.LEFT)
        self._pass_var = tk.StringVar()
        self._pass_var.trace_add("write", self._mark_modified)
        self._pass_entry = tk.Entry(pw_fr, textvariable=self._pass_var,
                                    show="*", width=28)
        self._pass_entry.pack(side=tk.LEFT, padx=(4, 0))
        tk.Button(pw_fr, text="表示", padx=4,
                  command=self._toggle_password).pack(side=tk.LEFT, padx=4)

        self._gas_var, _ = self._form_row(sec, "GAS URL:", width=56)

    def _build_device_section(self):
        sec = self._make_section("⚙ デバイス設定")
        fr = tk.Frame(sec, bg=SECTION_BG)
        fr.pack(fill=tk.X, pady=2)

        tk.Label(fr, text="デバイス ID:", width=16, anchor="e",
                 bg=SECTION_BG).pack(side=tk.LEFT)
        self._device_id_var = tk.StringVar()
        self._device_id_var.trace_add("write", self._mark_modified)
        tk.Entry(fr, textvariable=self._device_id_var, width=8).pack(
            side=tk.LEFT, padx=(4, 24))

        self._recording_var = tk.BooleanVar(value=True)
        tk.Checkbutton(fr, text="計量記録を有効にする（recording_enabled）",
                       variable=self._recording_var, bg=SECTION_BG,
                       command=self._mark_modified).pack(side=tk.LEFT)

    def _build_product_section(self):
        sec = self._make_section("🍊 製品設定")

        self._prod_name_var, _ = self._form_row(sec, "製品名:")

        for label, attr in [
            ("最小測定重量:", "_min_w_var"),
            ("最大測定重量:", "_max_w_var"),
            ("安定判定閾値:", "_thresh_var"),
        ]:
            fr = tk.Frame(sec, bg=SECTION_BG)
            fr.pack(fill=tk.X, pady=2)
            tk.Label(fr, text=label, width=16, anchor="e",
                     bg=SECTION_BG).pack(side=tk.LEFT)
            var = tk.StringVar()
            var.trace_add("write", self._mark_modified)
            setattr(self, attr, var)
            tk.Entry(fr, textvariable=var, width=10).pack(side=tk.LEFT, padx=(4, 0))
            tk.Label(fr, text="g", bg=SECTION_BG, fg="#555").pack(side=tk.LEFT, padx=(3, 0))

    def _build_grades_section(self):
        sec = self._make_section("📊 規格設定（grades）")

        # Toolbar
        tb = tk.Frame(sec, bg=SECTION_BG)
        tb.pack(fill=tk.X, pady=(0, 4))

        tk.Button(tb, text="＋ 追加", bg="#2a6bc5", fg="white",
                  activebackground="#1a5ab5", activeforeground="white",
                  padx=6, command=self._add_grade).pack(side=tk.LEFT)

        self._btn_edit = tk.Button(tb, text="✏ 編集", padx=6,
                                   state=tk.DISABLED, command=self._edit_grade)
        self._btn_edit.pack(side=tk.LEFT, padx=(4, 0))

        self._btn_delete = tk.Button(tb, text="🗑 削除", padx=6, fg="#990000",
                                     state=tk.DISABLED, command=self._delete_grade)
        self._btn_delete.pack(side=tk.LEFT, padx=(4, 0))

        ttk.Separator(tb, orient="vertical").pack(side=tk.LEFT, fill=tk.Y, padx=6)

        self._btn_up = tk.Button(tb, text="↑ 上へ", padx=6,
                                 state=tk.DISABLED, command=self._move_up)
        self._btn_up.pack(side=tk.LEFT)

        self._btn_down = tk.Button(tb, text="↓ 下へ", padx=6,
                                   state=tk.DISABLED, command=self._move_down)
        self._btn_down.pack(side=tk.LEFT, padx=(4, 0))

        tk.Label(tb, text="← ダブルクリックでも編集", fg="#999",
                 bg=SECTION_BG, font=("", 8)).pack(side=tk.RIGHT)

        # Treeview
        tree_fr = tk.Frame(sec)
        tree_fr.pack(fill=tk.X)

        cols = ("no", "name", "min_w", "max_w", "sound", "color")
        self._tree = ttk.Treeview(tree_fr, columns=cols, show="headings",
                                   height=12, selectmode="browse")
        self._tree.heading("no",    text="No.")
        self._tree.heading("name",  text="規格名")
        self._tree.heading("min_w", text="最小重量 (g)")
        self._tree.heading("max_w", text="最大重量 (g)")
        self._tree.heading("sound", text="音声ファイル")
        self._tree.heading("color", text="背景色")

        self._tree.column("no",    width=42,  anchor="center", stretch=False)
        self._tree.column("name",  width=70,  anchor="center", stretch=False)
        self._tree.column("min_w", width=95,  anchor="center", stretch=False)
        self._tree.column("max_w", width=95,  anchor="center", stretch=False)
        self._tree.column("sound", width=220, anchor="w")
        self._tree.column("color", width=120, anchor="center", stretch=False)

        self._vsb = ttk.Scrollbar(tree_fr, orient="vertical", command=self._tree.yview)
        self._tree.configure(yscrollcommand=self._on_tree_yscroll)
        self._tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        self._vsb.pack(side=tk.RIGHT, fill=tk.Y)

        self._tree.bind("<<TreeviewSelect>>", self._on_select)
        self._tree.bind("<Double-1>", lambda _e: self._edit_grade())
        self._tree.bind("<Configure>", lambda _e: self.after(10, self._draw_color_cells))

        # Range check message
        self._range_lbl = tk.Label(sec, text="", anchor="w", font=("", 9),
                                   padx=8, pady=4, bg=SECTION_BG)
        self._range_lbl.pack(fill=tk.X, pady=(4, 0))

    def _build_statusbar(self):
        bar = tk.Frame(self, bg="#e0e0e0", relief="sunken", bd=1)
        bar.pack(fill=tk.X, side=tk.BOTTOM)
        self._status_var = tk.StringVar(value="（ファイルが開かれていません）")
        tk.Label(bar, textvariable=self._status_var, anchor="w",
                 bg="#e0e0e0", fg="#444", font=("", 9)).pack(side=tk.LEFT, padx=8)
        self._count_var = tk.StringVar(value="規格数: 0")
        tk.Label(bar, textvariable=self._count_var, anchor="e",
                 bg="#e0e0e0", fg="#666", font=("", 9)).pack(side=tk.RIGHT, padx=8)

    # ================================================================
    #  Helpers
    # ================================================================

    def _toggle_password(self):
        self._pass_entry.config(
            show="" if self._pass_entry.cget("show") == "*" else "*")

    def _mark_modified(self, *_args):
        if not self._modified:
            self._modified = True
            self._status_var.set("● 未保存の変更あり")

    def _refresh_tree(self):
        self._tree.delete(*self._tree.get_children())
        for i, g in enumerate(self._grades):
            color = g.get("color", "#FFFFFF").upper()
            self._tree.insert("", "end", iid=str(i),
                               values=(i + 1, g["name"], g["min_weight"],
                                       g["max_weight"], g["sound_file"], color))
        self._count_var.set(f"規格数: {len(self._grades)}")
        self._check_ranges()
        self._update_buttons()
        self.after(20, self._draw_color_cells)

    def _update_buttons(self):
        sel = self._tree.selection()
        has = bool(sel)
        s = tk.NORMAL if has else tk.DISABLED
        self._btn_edit.config(state=s)
        self._btn_delete.config(state=s)
        if has:
            idx = int(sel[0])
            self._btn_up.config(
                state=tk.NORMAL if idx > 0 else tk.DISABLED)
            self._btn_down.config(
                state=tk.NORMAL if idx < len(self._grades) - 1 else tk.DISABLED)
        else:
            self._btn_up.config(state=tk.DISABLED)
            self._btn_down.config(state=tk.DISABLED)

    def _check_ranges(self):
        if not self._grades:
            self._range_lbl.config(text="", bg=SECTION_BG)
            return
        sorted_g = sorted(self._grades, key=lambda g: g["min_weight"])
        issues = []
        for i in range(len(sorted_g) - 1):
            a, b = sorted_g[i], sorted_g[i + 1]
            if a["max_weight"] >= b["min_weight"]:
                issues.append(
                    f"重複: 「{a['name']}」({a['min_weight']}–{a['max_weight']}) "
                    f"と 「{b['name']}」({b['min_weight']}–{b['max_weight']})")
            elif a["max_weight"] + 1 < b["min_weight"]:
                issues.append(
                    f"ギャップ: 「{a['name']}」と「{b['name']}」の間 "
                    f"({a['max_weight'] + 1}–{b['min_weight'] - 1}) が空いています")
        if not issues:
            self._range_lbl.config(
                text="✅ 範囲チェック: OK — 重複なし・ギャップなし",
                fg="#1b5e20", bg="#e8f5e9")
        else:
            has_overlap = any("重複" in s for s in issues)
            self._range_lbl.config(
                text="⚠ " + " / ".join(issues),
                fg="#7f0000" if has_overlap else "#5d3a00",
                bg="#ffebee" if has_overlap else "#fff8e1")

    def _selected_index(self):
        sel = self._tree.selection()
        return int(sel[0]) if sel else None

    def _on_tree_yscroll(self, first, last):
        """Scrollbar update + redraw color cells after scroll."""
        self._vsb.set(first, last)
        self.after(1, self._draw_color_cells)

    def _draw_color_cells(self, *_):
        """Overlay colored Canvas widgets on the 背景色 column only."""
        for c in self._color_canvases:
            c.destroy()
        self._color_canvases = []
        for iid in self._tree.get_children():
            bbox = self._tree.bbox(iid, "color")
            if not bbox:
                continue
            x, y, w, h = bbox
            idx = int(iid)
            color = self._grades[idx].get("color", "#FFFFFF").upper()
            fg = contrast_color(color)
            cnv = tk.Canvas(self._tree, bg=color, width=w, height=h,
                            highlightthickness=0, bd=0)
            cnv.place(x=x, y=y, width=w, height=h)
            cnv.create_text(w // 2, h // 2, text=color, fill=fg,
                            font=("Consolas", 9, "bold"))
            cnv.bind("<Button-1>",
                     lambda e, i=iid: (self._tree.selection_set(i),
                                       self._tree.focus(i)))
            cnv.bind("<Double-Button-1>", lambda e: self._edit_grade())
            self._color_canvases.append(cnv)

    def _on_select(self, _event=None):
        self._update_buttons()

    # ================================================================
    #  Grade CRUD
    # ================================================================

    def _add_grade(self):
        dlg = GradeDialog(self, "規格を追加")
        if dlg.result:
            self._grades.append(dlg.result)
            self._refresh_tree()
            self._mark_modified()

    def _edit_grade(self):
        idx = self._selected_index()
        if idx is None:
            return
        dlg = GradeDialog(self, "規格を編集", self._grades[idx])
        if dlg.result:
            self._grades[idx] = dlg.result
            self._refresh_tree()
            self._tree.selection_set(str(idx))
            self._mark_modified()

    def _delete_grade(self):
        idx = self._selected_index()
        if idx is None:
            return
        name = self._grades[idx]["name"]
        if not messagebox.askyesno("削除確認",
                                   f"規格「{name}」を削除しますか？", parent=self):
            return
        del self._grades[idx]
        self._refresh_tree()
        self._mark_modified()

    def _move_up(self):
        idx = self._selected_index()
        if idx is None or idx == 0:
            return
        self._grades[idx - 1], self._grades[idx] = \
            self._grades[idx], self._grades[idx - 1]
        self._refresh_tree()
        self._tree.selection_set(str(idx - 1))
        self._mark_modified()

    def _move_down(self):
        idx = self._selected_index()
        if idx is None or idx >= len(self._grades) - 1:
            return
        self._grades[idx + 1], self._grades[idx] = \
            self._grades[idx], self._grades[idx + 1]
        self._refresh_tree()
        self._tree.selection_set(str(idx + 1))
        self._mark_modified()

    # ================================================================
    #  File operations
    # ================================================================

    def _open_file(self):
        if self._modified:
            if not messagebox.askyesno(
                    "確認", "未保存の変更があります。続けますか？", parent=self):
                return
        path = filedialog.askopenfilename(
            filetypes=[("JSON files", "*.json"), ("All files", "*.*")],
            title="config.json を開く")
        if not path:
            return
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
        except Exception as e:
            messagebox.showerror("読み込みエラー",
                                 f"ファイルを読み込めませんでした。\n{e}", parent=self)
            return
        self._filepath = path
        self._data = data
        self._load_data(data)
        self._path_var.set(path)
        self._modified = False
        self._status_var.set("保存済み")

    def _load_data(self, data):
        wifi = data.get("wifi", {})
        self._ssid_var.set(wifi.get("ssid", ""))
        self._pass_var.set(wifi.get("password", ""))
        self._gas_var.set(data.get("gas_url", ""))
        self._device_id_var.set(str(data.get("device_id", "")))
        self._recording_var.set(data.get("recording_enabled", True))

        ps = data.get("product_settings", {})
        self._prod_name_var.set(ps.get("product_name", ""))
        self._min_w_var.set(str(ps.get("min_weight", "")))
        self._max_w_var.set(str(ps.get("max_weight", "")))
        self._thresh_var.set(str(ps.get("stability_threshold", "")))

        self._grades = list(ps.get("grades", []))
        self._refresh_tree()

    def _collect_data(self):
        """Validate all fields and return a JSON-ready dict, or None on error."""
        ssid    = self._ssid_var.get().strip()
        password= self._pass_var.get()
        gas_url = self._gas_var.get().strip()
        dev_id_s= self._device_id_var.get().strip()
        pname   = self._prod_name_var.get().strip()
        min_s   = self._min_w_var.get().strip()
        max_s   = self._max_w_var.get().strip()
        thr_s   = self._thresh_var.get().strip()

        errors = []
        if not ssid:
            errors.append("WiFi SSID を入力してください。")
        if not gas_url.startswith("https://"):
            errors.append("GAS URL は https:// で始まる必要があります。")
        try:
            dev_id = int(dev_id_s)
            if dev_id < 1:
                raise ValueError
        except ValueError:
            errors.append("デバイス ID は 1 以上の整数で入力してください。")
            dev_id = None
        if not pname:
            errors.append("製品名を入力してください。")
        try:
            min_w = float(min_s)
        except ValueError:
            errors.append("最小測定重量は数値で入力してください。")
            min_w = None
        try:
            max_w = float(max_s)
        except ValueError:
            errors.append("最大測定重量は数値で入力してください。")
            max_w = None
        if min_w is not None and max_w is not None and min_w >= max_w:
            errors.append("最大測定重量は最小測定重量より大きくしてください。")
        try:
            thresh = float(thr_s)
            if thresh <= 0:
                raise ValueError
        except ValueError:
            errors.append("安定判定閾値は 0 より大きい数値で入力してください。")
            thresh = None

        if errors:
            messagebox.showerror("入力エラー", "\n".join(errors), parent=self)
            return None

        data = dict(self._data) if self._data else {}
        data["wifi"] = {"ssid": ssid, "password": password}
        data["gas_url"] = gas_url
        data["device_id"] = dev_id
        data["recording_enabled"] = self._recording_var.get()
        data["product_settings"] = {
            "product_name": pname,
            "min_weight": min_w,
            "max_weight": max_w,
            "stability_threshold": thresh,
            "grades": self._grades,
        }
        return data

    def _save(self):
        if not self._filepath:
            self._save_as()
            return
        data = self._collect_data()
        if data is not None:
            self._write_file(self._filepath, data)

    def _save_as(self):
        data = self._collect_data()
        if data is None:
            return
        path = filedialog.asksaveasfilename(
            defaultextension=".json",
            filetypes=[("JSON files", "*.json"), ("All files", "*.*")],
            title="名前を付けて保存")
        if not path:
            return
        self._filepath = path
        self._path_var.set(path)
        self._write_file(path, data)

    def _write_file(self, path, data):
        try:
            with open(path, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
                f.write("\n")
            self._data = data
            self._modified = False
            self._status_var.set("保存済み")
        except Exception as e:
            messagebox.showerror("保存エラー", f"保存できませんでした。\n{e}", parent=self)

    def _on_close(self):
        if self._modified:
            if not messagebox.askyesno(
                    "終了確認", "未保存の変更があります。終了しますか？", parent=self):
                return
        self.destroy()


if __name__ == "__main__":
    app = ConfigEditor()
    app.mainloop()
