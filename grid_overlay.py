"""
Overlay a 32x24 character grid on a TI-99 screen PNG, with zoom.

Adjust X/Y offset and cell width/height to align the grid to the
character boundaries. Zoom in/out to inspect details; calibrate by
clicking the top-left then bottom-right of the character area.

Controls:
    Mouse wheel        : scroll vertically
    Shift + wheel      : scroll horizontally
    Ctrl  + wheel      : zoom in/out
    +, -, 1:1, Fit     : zoom buttons

Usage:
    python grid_overlay.py [path-to-png]

Defaults to ti99_00_boot.png in the script's directory.
"""

import os
import sys
import tkinter as tk
from tkinter import ttk, filedialog

from PIL import Image, ImageTk

DEFAULT_COLS = 32
DEFAULT_ROWS = 24
GRID_COLOR = "#ff3030"
HIGHLIGHT_COLOR = "#30ff30"
PAD = 8

ZOOM_LEVELS = [0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0]
VIEWPORT_MAX_W = 1100
VIEWPORT_MAX_H = 800


class GridOverlay:
    def __init__(self, root, image_path):
        self.root = root
        self.root.title("TI-99 Grid Overlay")

        self.image_path = image_path
        self.pil_image = Image.open(image_path).convert("RGB")
        self.img_w, self.img_h = self.pil_image.size
        self.tk_image = None
        self.canvas_image = None
        self.zoom = 1.0

        self.cols = DEFAULT_COLS
        self.rows = DEFAULT_ROWS
        self.off_x = tk.DoubleVar(value=0.0)
        self.off_y = tk.DoubleVar(value=0.0)
        self.cell_w = tk.DoubleVar(value=self.img_w / self.cols)
        self.cell_h = tk.DoubleVar(value=self.img_h / self.rows)
        self.cols_var = tk.IntVar(value=self.cols)
        self.rows_var = tk.IntVar(value=self.rows)

        controls = ttk.Frame(root)
        controls.grid(row=0, column=0, sticky="we", padx=PAD, pady=PAD)

        self._add_spin(controls, "Cols", self.cols_var, 1, 128, 0, self.on_grid_changed)
        self._add_spin(controls, "Rows", self.rows_var, 1, 128, 2, self.on_grid_changed)
        self._add_spin(controls, "Off X", self.off_x, -400, 400, 4, self.redraw_grid, is_float=True)
        self._add_spin(controls, "Off Y", self.off_y, -400, 400, 6, self.redraw_grid, is_float=True)
        self._add_spin(controls, "Cell W", self.cell_w, 1, 400, 8, self.redraw_grid, is_float=True)
        self._add_spin(controls, "Cell H", self.cell_h, 1, 400, 10, self.redraw_grid, is_float=True)

        ttk.Button(controls, text="Auto-fit", command=self.auto_fit).grid(
            row=0, column=12, padx=(PAD, 0)
        )
        ttk.Button(controls, text="Calibrate", command=self.start_calibration).grid(
            row=0, column=13, padx=(PAD, 0)
        )
        ttk.Button(controls, text="Open...", command=self.open_file).grid(
            row=0, column=14, padx=(PAD, 0)
        )
        ttk.Button(controls, text="Save PNG", command=self.save_png).grid(
            row=0, column=15, padx=(PAD, 0)
        )

        zoom_frame = ttk.Frame(root)
        zoom_frame.grid(row=1, column=0, sticky="w", padx=PAD, pady=(0, 4))
        ttk.Button(zoom_frame, text="\u2212", width=3, command=self.zoom_out).pack(side="left", padx=2)
        ttk.Button(zoom_frame, text="+", width=3, command=self.zoom_in).pack(side="left", padx=2)
        ttk.Button(zoom_frame, text="1:1", width=4, command=self.zoom_reset).pack(side="left", padx=2)
        ttk.Button(zoom_frame, text="Fit", width=4, command=self.zoom_fit).pack(side="left", padx=2)
        self.zoom_var = tk.StringVar(value="Zoom: 100%")
        ttk.Label(zoom_frame, textvariable=self.zoom_var).pack(side="left", padx=(PAD, 0))

        self.calibration_stage = 0
        self.calibration_tl = None

        canvas_frame = ttk.Frame(root)
        canvas_frame.grid(row=2, column=0, sticky="nsew", padx=PAD, pady=(0, PAD))
        root.grid_rowconfigure(2, weight=1)
        root.grid_columnconfigure(0, weight=1)

        vp_w = min(self.img_w, VIEWPORT_MAX_W)
        vp_h = min(self.img_h, VIEWPORT_MAX_H)
        self.canvas = tk.Canvas(
            canvas_frame,
            width=vp_w,
            height=vp_h,
            bg="black",
            highlightthickness=0,
        )
        self.hbar = ttk.Scrollbar(canvas_frame, orient="horizontal", command=self.canvas.xview)
        self.vbar = ttk.Scrollbar(canvas_frame, orient="vertical", command=self.canvas.yview)
        self.canvas.config(xscrollcommand=self.hbar.set, yscrollcommand=self.vbar.set)
        self.canvas.grid(row=0, column=0, sticky="nsew")
        self.vbar.grid(row=0, column=1, sticky="ns")
        self.hbar.grid(row=1, column=0, sticky="we")
        canvas_frame.grid_rowconfigure(0, weight=1)
        canvas_frame.grid_columnconfigure(0, weight=1)

        self.grid_items = []
        self.highlight_item = None

        self.canvas.bind("<Motion>", self.on_mouse_move)
        self.canvas.bind("<Leave>", lambda _e: self.clear_highlight())
        self.canvas.bind("<Button-1>", self.on_canvas_click)
        self.canvas.bind("<MouseWheel>", self.on_wheel)
        self.canvas.bind("<Shift-MouseWheel>", self.on_shift_wheel)
        self.canvas.bind("<Control-MouseWheel>", self.on_ctrl_wheel)

        self.status_var = tk.StringVar(value="")
        ttk.Label(root, textvariable=self.status_var).grid(
            row=3, column=0, sticky="w", padx=PAD, pady=(0, PAD)
        )

        self.update_display()
        self.redraw_grid()

    def _add_spin(self, parent, label, var, lo, hi, col, cb, is_float=False):
        ttk.Label(parent, text=label + ":").grid(row=0, column=col, padx=(0, 2))
        increment = 0.1 if is_float else 1
        width = 7 if is_float else 4
        sp = ttk.Spinbox(
            parent, from_=lo, to=hi, width=width,
            increment=increment, textvariable=var, command=cb,
        )
        sp.grid(row=0, column=col + 1, padx=(0, PAD))
        sp.bind("<Return>", lambda _e: cb())
        sp.bind("<FocusOut>", lambda _e: cb())

    # --- zoom / display ---

    def update_display(self):
        w = max(1, int(round(self.img_w * self.zoom)))
        h = max(1, int(round(self.img_h * self.zoom)))
        resample = Image.NEAREST if self.zoom >= 1.0 else Image.BOX
        resized = self.pil_image.resize((w, h), resample)
        self.tk_image = ImageTk.PhotoImage(resized)
        if self.canvas_image is None:
            self.canvas_image = self.canvas.create_image(0, 0, anchor="nw", image=self.tk_image)
        else:
            self.canvas.itemconfig(self.canvas_image, image=self.tk_image)
        self.canvas.config(scrollregion=(0, 0, w, h))
        self.zoom_var.set(f"Zoom: {self.zoom * 100:.0f}%")

    def set_zoom(self, z, anchor_xy=None):
        z = max(ZOOM_LEVELS[0], min(ZOOM_LEVELS[-1], z))
        if anchor_xy is not None:
            orig_x, orig_y = anchor_xy
        else:
            orig_x = self.canvas.canvasx(self.canvas.winfo_width() / 2) / self.zoom
            orig_y = self.canvas.canvasy(self.canvas.winfo_height() / 2) / self.zoom
        self.zoom = z
        self.update_display()
        self.redraw_grid()
        self.clear_highlight()
        vp_w = self.canvas.winfo_width()
        vp_h = self.canvas.winfo_height()
        new_w = max(1, self.img_w * self.zoom)
        new_h = max(1, self.img_h * self.zoom)
        fx = max(0.0, min(1.0, (orig_x * self.zoom - vp_w / 2) / new_w))
        fy = max(0.0, min(1.0, (orig_y * self.zoom - vp_h / 2) / new_h))
        self.canvas.xview_moveto(fx)
        self.canvas.yview_moveto(fy)

    def zoom_in(self):
        for z in ZOOM_LEVELS:
            if z > self.zoom + 1e-6:
                self.set_zoom(z)
                return

    def zoom_out(self):
        for z in reversed(ZOOM_LEVELS):
            if z < self.zoom - 1e-6:
                self.set_zoom(z)
                return

    def zoom_reset(self):
        self.set_zoom(1.0)

    def zoom_fit(self):
        vp_w = self.canvas.winfo_width()
        vp_h = self.canvas.winfo_height()
        if vp_w < 2 or vp_h < 2:
            return
        z = min(vp_w / self.img_w, vp_h / self.img_h)
        self.set_zoom(z)

    def on_wheel(self, event):
        self.canvas.yview_scroll(-1 if event.delta > 0 else 1, "units")

    def on_shift_wheel(self, event):
        self.canvas.xview_scroll(-1 if event.delta > 0 else 1, "units")

    def on_ctrl_wheel(self, event):
        orig = self._orig_xy(event)
        if event.delta > 0:
            for z in ZOOM_LEVELS:
                if z > self.zoom + 1e-6:
                    self.set_zoom(z, anchor_xy=orig)
                    return
        else:
            for z in reversed(ZOOM_LEVELS):
                if z < self.zoom - 1e-6:
                    self.set_zoom(z, anchor_xy=orig)
                    return

    # --- coordinate helpers ---

    def _orig_xy(self, event):
        x = self.canvas.canvasx(event.x) / self.zoom
        y = self.canvas.canvasy(event.y) / self.zoom
        return x, y

    # --- grid ---

    def on_grid_changed(self):
        try:
            self.cols = max(1, int(self.cols_var.get()))
            self.rows = max(1, int(self.rows_var.get()))
        except (ValueError, tk.TclError):
            return
        self.redraw_grid()

    def auto_fit(self):
        self.off_x.set(0.0)
        self.off_y.set(0.0)
        self.cell_w.set(self.img_w / self.cols)
        self.cell_h.set(self.img_h / self.rows)
        self.redraw_grid()

    def redraw_grid(self):
        try:
            ox = float(self.off_x.get()) * self.zoom
            oy = float(self.off_y.get()) * self.zoom
            cw = float(self.cell_w.get()) * self.zoom
            ch = float(self.cell_h.get()) * self.zoom
        except (ValueError, tk.TclError):
            return

        for item in self.grid_items:
            self.canvas.delete(item)
        self.grid_items = []

        for i in range(self.cols + 1):
            x = round(ox + i * cw)
            self.grid_items.append(
                self.canvas.create_line(x, round(oy), x, round(oy + self.rows * ch),
                                        fill=GRID_COLOR, width=1)
            )
        for j in range(self.rows + 1):
            y = round(oy + j * ch)
            self.grid_items.append(
                self.canvas.create_line(round(ox), y, round(ox + self.cols * cw), y,
                                        fill=GRID_COLOR, width=1)
            )

        self.clear_highlight()

    def cell_from_orig(self, x, y):
        try:
            ox = float(self.off_x.get())
            oy = float(self.off_y.get())
            cw = float(self.cell_w.get())
            ch = float(self.cell_h.get())
        except (ValueError, tk.TclError):
            return None
        if cw <= 0 or ch <= 0:
            return None
        col = int((x - ox) // cw)
        row = int((y - oy) // ch)
        if 0 <= col < self.cols and 0 <= row < self.rows:
            return col, row, ox + col * cw, oy + row * ch, cw, ch
        return None

    def on_mouse_move(self, event):
        ox, oy = self._orig_xy(event)
        info = self.cell_from_orig(ox, oy)
        if info is None:
            self.clear_highlight()
            self.status_var.set(
                f"orig x={ox:.1f} y={oy:.1f}  (outside grid)"
            )
            return
        col, row, cx, cy, cw, ch = info
        self.status_var.set(
            f"col={col} row={row}   orig ({ox:.1f},{oy:.1f})   cell top-left ({round(cx)},{round(cy)})"
        )
        z = self.zoom
        x1, y1, x2, y2 = cx * z, cy * z, (cx + cw) * z, (cy + ch) * z
        if self.highlight_item is None:
            self.highlight_item = self.canvas.create_rectangle(
                x1, y1, x2, y2, outline=HIGHLIGHT_COLOR, width=2,
            )
        else:
            self.canvas.coords(self.highlight_item, x1, y1, x2, y2)

    def clear_highlight(self):
        if self.highlight_item is not None:
            self.canvas.delete(self.highlight_item)
            self.highlight_item = None

    def start_calibration(self):
        self.calibration_stage = 1
        self.calibration_tl = None
        self.canvas.config(cursor="crosshair")
        self.status_var.set(
            "Calibrate: click the TOP-LEFT corner of the character area"
        )

    def on_canvas_click(self, event):
        if self.calibration_stage == 0:
            return
        ox, oy = self._orig_xy(event)
        if self.calibration_stage == 1:
            self.calibration_tl = (ox, oy)
            self.calibration_stage = 2
            self.status_var.set(
                f"TL=({ox:.1f},{oy:.1f}) — now click the BOTTOM-RIGHT corner"
            )
        elif self.calibration_stage == 2:
            x1, y1 = self.calibration_tl
            x2, y2 = ox, oy
            if x2 <= x1 or y2 <= y1:
                self.status_var.set("Bottom-right must be below-right of top-left — try again")
                self.calibration_stage = 1
                return
            self.off_x.set(round(x1, 2))
            self.off_y.set(round(y1, 2))
            self.cell_w.set(round((x2 - x1) / self.cols, 3))
            self.cell_h.set(round((y2 - y1) / self.rows, 3))
            self.calibration_stage = 0
            self.calibration_tl = None
            self.canvas.config(cursor="")
            self.redraw_grid()
            self.status_var.set(
                f"Calibrated: off=({x1:.1f},{y1:.1f}) cell=({self.cell_w.get():.3f}x{self.cell_h.get():.3f})"
            )

    def open_file(self):
        path = filedialog.askopenfilename(
            filetypes=[("PNG images", "*.png"), ("All files", "*.*")]
        )
        if not path:
            return
        self.image_path = path
        self.pil_image = Image.open(path).convert("RGB")
        self.img_w, self.img_h = self.pil_image.size
        self.zoom = 1.0
        self.update_display()
        self.auto_fit()

    def save_png(self):
        try:
            ox = float(self.off_x.get())
            oy = float(self.off_y.get())
            cw = float(self.cell_w.get())
            ch = float(self.cell_h.get())
        except (ValueError, tk.TclError):
            return
        from PIL import ImageDraw
        out = self.pil_image.copy()
        draw = ImageDraw.Draw(out)
        for i in range(self.cols + 1):
            x = round(ox + i * cw)
            draw.line([(x, round(oy)), (x, round(oy + self.rows * ch))],
                      fill=(255, 48, 48), width=1)
        for j in range(self.rows + 1):
            y = round(oy + j * ch)
            draw.line([(round(ox), y), (round(ox + self.cols * cw), y)],
                      fill=(255, 48, 48), width=1)
        base, _ext = os.path.splitext(self.image_path)
        default = base + "_grid.png"
        path = filedialog.asksaveasfilename(
            defaultextension=".png",
            initialfile=os.path.basename(default),
            filetypes=[("PNG images", "*.png")],
        )
        if not path:
            return
        out.save(path)
        self.status_var.set(f"Saved: {path}")


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    default_path = os.path.join(script_dir, "ti99_00_boot.png")
    path = sys.argv[1] if len(sys.argv) > 1 else default_path
    if not os.path.isfile(path):
        print(f"Image not found: {path}")
        sys.exit(1)

    root = tk.Tk()
    root.minsize(600, 400)
    GridOverlay(root, path)
    root.mainloop()


if __name__ == "__main__":
    main()
