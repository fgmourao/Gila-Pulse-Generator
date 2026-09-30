"""
Py_GilaMonster_Monitor.py
=========================
PROJECT : Gila Monster — Pulse generator
VERSION : 1.0
AUTHOR  : Flávio Mourão — Sep, 2026
Last update : Sep, 2026

════════════════════════════════════════════════════════════════════════════════
MODULE RESPONSIBILITY
════════════════════════════════════════════════════════════════════════════════
Reads the Comm report of the Gila Monster pulse generator over USB-Serial and
displays it. The application is passive: nothing is ever written to the port.

  - Session report: parameters, pulse count and timing faults
  - Raw serial log, for anything the parser does not recognise
  - Inter-stimulus intervals, linear and log-log, for the three output types
  - Export of the interval values (.txt) and of the plots (.png)

In NPS mode the firmware simulates the interval sequence with the current
parameters; it is a new draw, not the sequence delivered during the session. In
Continuous and Burst the intervals are reconstructed from the programmed
parameters and the session pulse count.

════════════════════════════════════════════════════════════════════════════════
AUTO-RESET ON CONNECT
════════════════════════════════════════════════════════════════════════════════
On the Arduino Uno the DTR line is wired to the reset circuit, so opening the
port reboots the board. This is what lets the IDE upload firmware without
pressing the button, and what aborts a running session when the Serial Monitor
is opened.

The pulse comes from the operating system driver, not from the application, so
it cannot be fully suppressed from Python. What this module does:

  - clears DTR and RTS around the open
  - clears HUPCL on the device (POSIX), which suppresses the reset on every
    open after the first one
  - lists /dev/cu.* first on macOS, which does not force DTR on open

To avoid the reset altogether, connect BEFORE switching the generator ON, or fit
a 10 uF capacitor between RESET and GND (remove it to upload new firmware).

════════════════════════════════════════════════════════════════════════════════
REPORT FORMAT  (produced by Comm in the firmware)
════════════════════════════════════════════════════════════════════════════════
  === Gila Monster Settings ===        block start
  Mode:  Manual                        parameters, one per line
  Type:  NPS
  ...
  --- Last session ---
  Pulses fired:  1200                  session counters
  Timing faults: 0                     0 = every edge placed on time
  =============================        block end (non-NPS reports end here)

  >>> NPS RAW ITI DATA (...) <<<       NPS only: interval sequence
  [ITI_ms]
  154                                  one interval per line, in milliseconds
  ...
  >>> End of Data <<<                  end of an NPS report

════════════════════════════════════════════════════════════════════════════════
EXPORTED FILES
════════════════════════════════════════════════════════════════════════════════
  *_iti.txt   — the parameters as a comment header, then one interval per line
  *_iti.png   — both plots, as displayed
  *_log.txt   — the left pane as displayed: the reports, or the raw serial log

  Files are written only when the corresponding button is clicked.

════════════════════════════════════════════════════════════════════════════════
DEPENDENCIES
════════════════════════════════════════════════════════════════════════════════
  pyserial    >= 3.5   — pip install pyserial
  PyQt5       >= 5.15  — pip install PyQt5
  numpy       >= 1.20  — pip install numpy
  matplotlib  >= 3.5   — pip install matplotlib

  Compatible with Spyder IDE: uses QApplication.instance() to avoid creating
  a second QApplication when IPython already manages one.

════════════════════════════════════════════════════════════════════════════════
HARDWARE CONTEXT
════════════════════════════════════════════════════════════════════════════════
  Board  : Arduino Uno R3 (ATmega328P)
  USB    : ATmega16U2 USB-Serial bridge
  Baud   : 9600
  Stream : text report, sent on demand by the Comm menu item
"""

import os
import re
import sys
import time

import serial
import serial.tools.list_ports

from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QPushButton, QComboBox, QPlainTextEdit, QSplitter,
    QFileDialog, QCheckBox, QFrame, QMessageBox
)
from PyQt5.QtCore import QThread, pyqtSignal, Qt
from PyQt5.QtGui import QFont

import matplotlib
matplotlib.use("Qt5Agg")
from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg
from matplotlib.figure import Figure
import numpy as np

# ── Colour scheme ────────────────────────────────────────────────────────────
BG_APP    = "#000000"
BG_PANEL  = "#111111"
BG_PLOT   = "#0d0d0d"
FG_TEXT   = "#cccccc"
FG_MUTED  = "#666666"
GRID      = "#2a2a2a"
ACCENT    = "#44aaff"   # fit line, headings
DATA      = "#cc3333"   # interval data
ALERT     = "#ff8c00"   # timing faults present

# ── Report grammar ───────────────────────────────────────────────────────────
BLOCK_START = "=== Gila Monster Settings ==="
BLOCK_END   = "============================="
ITI_START   = ">>> NPS RAW ITI DATA"
ITI_END     = ">>> End of Data <<<"

FIELDS = {
    "Mode": "Mode", "Edge": "Trigger edge", "Type": "Type",
    "Freq": "Frequency", "Count": "Pulse count", "Gap": "Gap",
    "Min": "Minimum ITI", "PortB": "Port B", "Width": "Width mode",
    "Pulse": "Pulse width", "Timer": "Timer",
    "Pulses fired": "Pulses fired", "Timing faults": "Timing faults",
}


# ════════════════════════════════════════════════════════════════════════════
# REPORT AND INCREMENTAL PARSER
# ════════════════════════════════════════════════════════════════════════════

class Report:
    """One Comm report: parameters, session counters and interval data."""

    def __init__(self):
        self.time_label = time.strftime("%H:%M:%S")
        self.fields = {}          # display label -> value, insertion ordered
        self.iti_ms = []

    def feed(self, line):
        if ":" in line:
            label, _, value = line.partition(":")
            label, value = label.strip(), value.strip()
            if label in FIELDS:
                self.fields[FIELDS[label]] = value

    def add_iti(self, line):
        text = line.strip()
        if text.isdigit():
            self.iti_ms.append(int(text))

    def is_nps(self):
        return self.fields.get("Type", "").upper().startswith("NPS")

    def faults(self):
        return self.fields.get("Timing faults", "?")

    def interval_data(self):
        """
        Return (intervals_ms, source_label), or (None, None).

        NPS        — the sequence exported by the firmware.
        Continuous — the period, repeated (pulses fired - 1) times.
        Burst      — the period inside a train, and the pulse width plus the
                     Gap between trains.
        """
        if self.iti_ms:
            return list(self.iti_ms), "simulated by the firmware"

        kind = self.fields.get("Type", "").lower()
        pulses = _number(self.fields.get("Pulses fired"), 0) or 0
        period_us = _parenthesised_us(self.fields.get("Frequency"))
        if not period_us or pulses < 2:
            return None, None
        period_ms = period_us / 1000.0

        if kind.startswith("cont"):
            return [period_ms] * int(pulses - 1), "reconstructed from the parameters"

        if kind.startswith("burst"):
            count = _number(self.fields.get("Pulse count"), 0) or 0
            gap_ms = _number(self.fields.get("Gap"), 0) or 0
            width_us = _parenthesised_us(self.fields.get("Pulse width"), 0) or 0
            if count < 1:
                return None, None
            trains = max(int(pulses // count), 1)
            intra = int(pulses - trains)                 # within the trains
            inter = max(trains - 1, 0)                   # between trains
            values = [period_ms] * intra + [gap_ms + width_us / 1000.0] * inter
            return (values, "reconstructed from the parameters") if values else (None, None)

        return None, None

    def as_text(self):
        width = max((len(k) for k in self.fields), default=10)
        out = [f"  Received        {self.time_label}"]
        out += [f"  {k.ljust(width)}   {v}" for k, v in self.fields.items()]
        if self.iti_ms:
            out.append(f"  {'ITI samples'.ljust(width)}   {len(self.iti_ms)}")
        return "\n".join(out)


def _number(text, default=None):
    """First number in a string: '20.00 Hz (period 50000 us)' -> 20.0"""
    if not text:
        return default
    m = re.search(r"-?\d+(?:\.\d+)?", text)
    return float(m.group()) if m else default


def _parenthesised_us(text, default=None):
    """Value in microseconds inside parentheses: '(HIGH time 2000 us)' -> 2000.0"""
    if not text:
        return default
    m = re.search(r"\((?:period|HIGH time)\s+(\d+)\s*us\)", text)
    return float(m.group(1)) if m else default


class StreamParser:
    """Feed one line at a time; returns a Report when a block completes."""

    def __init__(self):
        self.report = None
        self.in_iti = False

    def feed(self, line):
        text = line.rstrip("\r\n")
        if BLOCK_START in text:
            self.report, self.in_iti = Report(), False
            return None
        if self.report is None:
            return None
        if ITI_END in text:
            done, self.report, self.in_iti = self.report, None, False
            return done
        if text.startswith(ITI_START):
            self.in_iti = True
            return None
        if self.in_iti:
            self.report.add_iti(text)
            return None
        if BLOCK_END in text:
            if not self.report.is_nps():          # NPS reports continue into the data
                done, self.report = self.report, None
                return done
            return None
        self.report.feed(text)
        return None


def disable_hupcl(port):
    """
    Clear HUPCL on the device (POSIX only); ignored where unsupported.

    With HUPCL set, the driver drops DTR on close, and the next open produces
    the transition that resets the board.
    """
    if os.name != "posix":
        return
    try:
        import termios
        fd = os.open(port, os.O_RDONLY | os.O_NONBLOCK)
        try:
            attrs = termios.tcgetattr(fd)
            attrs[2] &= ~termios.HUPCL
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
        finally:
            os.close(fd)
    except Exception:
        pass


# ════════════════════════════════════════════════════════════════════════════
# SERIAL READING THREAD
# ════════════════════════════════════════════════════════════════════════════

class SerialThread(QThread):
    """
    Owns the serial port and reads from it. Never writes to it.

    DTR and RTS are cleared around the open to limit the auto-reset; see the
    module header for what that does and does not achieve.
    """

    line_received    = pyqtSignal(str)
    connection_error = pyqtSignal(str)

    def __init__(self, port, baudrate=9600):
        super().__init__()
        self.port     = port
        self.baudrate = baudrate
        self.ser      = None
        self.running  = True

    def run(self):
        try:
            disable_hupcl(self.port)
            self.ser = serial.Serial()
            self.ser.port     = self.port
            self.ser.baudrate = self.baudrate
            self.ser.timeout  = 1
            self.ser.dtr      = False
            self.ser.rts      = False
            self.ser.open()
            self.ser.dtr      = False
            self.ser.rts      = False

            while self.running:
                raw = self.ser.readline()
                if raw:
                    self.line_received.emit(raw.decode("utf-8", errors="replace"))

        except Exception as e:
            if self.running:
                self.connection_error.emit(str(e))
        finally:
            if self.ser and self.ser.is_open:
                self.ser.close()

    def stop(self):
        self.running = False


class DemoThread(QThread):
    """Replays a saved log file, to test the interface without hardware."""

    line_received    = pyqtSignal(str)
    connection_error = pyqtSignal(str)

    def __init__(self, path):
        super().__init__()
        self.path    = path
        self.running = True

    def run(self):
        try:
            lines = open(self.path, errors="replace").readlines()
        except Exception as e:
            self.connection_error.emit(str(e))
            return
        for line in lines:
            if not self.running:
                break
            self.line_received.emit(line)
            time.sleep(0.002)

    def stop(self):
        self.running = False


# ════════════════════════════════════════════════════════════════════════════
# MAIN APPLICATION WINDOW
# ════════════════════════════════════════════════════════════════════════════

class GilaMonitor(QMainWindow):
    """
    Main window.

    Top bar    — port selector, connect, raw-log toggle, status
    Left pane  — session reports, or the raw serial log
    Right pane — interval plots and the export buttons
    """

    def __init__(self, demo_file=None):
        super().__init__()
        self.setWindowTitle("Gila Monster — Serial Monitor")
        self.resize(1250, 760)
        self.setStyleSheet(f"background-color: {BG_APP}; color: {FG_TEXT};")

        self.demo_file  = demo_file
        self.thread     = None
        self.parser     = StreamParser()
        self.reports    = []
        self.current    = None
        self.raw_lines  = []
        self.current_values = None
        self.current_source = None

        self.init_ui()
        self.refresh_ports()

    # ─────────────────────────────────────────────────────────────────────────
    # UI CONSTRUCTION
    # ─────────────────────────────────────────────────────────────────────────

    def init_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        main_layout = QVBoxLayout(central)

        # ── Top bar ───────────────────────────────────────────────────────────
        top_bar = QHBoxLayout()

        self.combo_ports = QComboBox()
        self.combo_ports.setMinimumWidth(300)
        self.combo_ports.setStyleSheet(
            "background-color: #222; color: #fff; padding: 5px; font-size: 13px;")

        self.btn_refresh = QPushButton("REFRESH")
        self.btn_refresh.setStyleSheet(
            "background-color: #333; color: #aaa; padding: 8px; "
            "font-weight: bold; border: none; border-radius: 4px;")
        self.btn_refresh.clicked.connect(self.refresh_ports)

        self.btn_connect = QPushButton("CONNECT")
        self.btn_connect.setStyleSheet(
            "background-color: #0055a4; color: #fff; padding: 8px; "
            "font-weight: bold; border: none; border-radius: 4px;")
        self.btn_connect.clicked.connect(self.toggle_connection)

        self.chk_raw = QCheckBox("Raw serial log")
        self.chk_raw.setStyleSheet(f"color: {FG_MUTED};")
        self.chk_raw.stateChanged.connect(self.refresh_text)

        self.lbl_status = QLabel("Not connected")
        self.lbl_status.setStyleSheet(f"color: {FG_MUTED}; font-size: 13px; margin-left: 20px;")

        top_bar.addWidget(QLabel("USB Port:"))
        top_bar.addWidget(self.combo_ports)
        top_bar.addWidget(self.btn_refresh)
        top_bar.addWidget(self.btn_connect)
        top_bar.addWidget(self.chk_raw)
        top_bar.addWidget(self.lbl_status)
        top_bar.addStretch()
        main_layout.addLayout(top_bar)

        

        # ── Body: text on the left, plots on the right ────────────────────────
        splitter = QSplitter(Qt.Horizontal)

        # Left — report text
        left = QFrame()
        left.setStyleSheet(f"QFrame {{ background-color: {BG_PANEL}; border: none; "
                           "border-radius: 4px; }")
        left_box = QVBoxLayout(left)

        lbl_left = QLabel("SESSION REPORT")
        lbl_left.setStyleSheet(f"color: {FG_MUTED}; font-size: 12px; font-weight: bold; "
                               "letter-spacing: 1px; border: none;")
        left_box.addWidget(lbl_left)

        self.txt_report = QPlainTextEdit()
        self.txt_report.setReadOnly(True)
        self.txt_report.setMaximumBlockCount(20000)
        self.txt_report.setFont(QFont("Menlo", 11))
        self.txt_report.setStyleSheet(
            f"QPlainTextEdit {{ background-color: {BG_PLOT}; color: {FG_TEXT}; "
            "border: none; padding: 6px; }}")
        left_box.addWidget(self.txt_report)

        left_btn_row = QHBoxLayout()
        self.btn_log = QPushButton("SAVE LOG (.txt)")
        self.btn_clear = QPushButton("CLEAR SESSIONS")
        for btn, slot in ((self.btn_log, self.save_log), (self.btn_clear, self.clear_sessions)):
            btn.setStyleSheet(
                "background-color: #333; color: #aaa; padding: 8px; "
                "font-weight: bold; border: none; border-radius: 4px;")
            btn.clicked.connect(slot)
            left_btn_row.addWidget(btn)
        left_box.addLayout(left_btn_row)
        splitter.addWidget(left)

        # Right — plots and exports
        right = QFrame()
        right.setStyleSheet(f"QFrame {{ background-color: {BG_PANEL}; border: none; "
                            "border-radius: 4px; }")
        right_box = QVBoxLayout(right)

        lbl_right = QLabel("INTER-STIMULUS INTERVALS")
        lbl_right.setStyleSheet(f"color: {FG_MUTED}; font-size: 12px; font-weight: bold; "
                                "letter-spacing: 1px; border: none;")
        right_box.addWidget(lbl_right)

        self.figure = Figure(figsize=(7, 6), facecolor=BG_PANEL)
        self.canvas = FigureCanvasQTAgg(self.figure)
        right_box.addWidget(self.canvas)

        btn_row = QHBoxLayout()
        self.btn_txt = QPushButton("SAVE INTERVALS (.txt)")
        self.btn_png = QPushButton("SAVE PLOTS (.png)")
        for btn, slot in ((self.btn_txt, self.save_txt), (self.btn_png, self.save_png)):
            btn.setStyleSheet(
                "background-color: #333; color: #aaa; padding: 8px; "
                "font-weight: bold; border: none; border-radius: 4px;")
            btn.setEnabled(False)
            btn.clicked.connect(slot)
            btn_row.addWidget(btn)
        right_box.addLayout(btn_row)
        splitter.addWidget(right)

        splitter.setStretchFactor(0, 4)
        splitter.setStretchFactor(1, 5)
        main_layout.addWidget(splitter)

        self.clear_plots("Waiting for a report.")

    # ─────────────────────────────────────────────────────────────────────────
    # CONNECTION MANAGEMENT
    # ─────────────────────────────────────────────────────────────────────────

    def refresh_ports(self):
        self.combo_ports.clear()
        if self.demo_file:
            self.combo_ports.addItem(f"demo: {os.path.basename(self.demo_file)}")
            return
        ports = list(serial.tools.list_ports.comports())
        if not ports:
            self.combo_ports.addItem("no ports found")
            return
        # macOS: /dev/cu.* does not force DTR on open, unlike /dev/tty.*
        ports.sort(key=lambda p: (not p.device.startswith("/dev/cu."), p.device))
        for p in ports:
            self.combo_ports.addItem(f"{p.device}  —  {p.description}", p.device)

    def toggle_connection(self):
        if self.thread is not None and self.thread.running:
            self.disconnect_port()
        else:
            self.connect_port()

    def connect_port(self):
        if self.demo_file:
            self.thread = DemoThread(self.demo_file)
            name = os.path.basename(self.demo_file)
        else:
            port = self.combo_ports.currentData()
            if not port:
                self.lbl_status.setText("Select a valid port")
                return
            self.thread = SerialThread(port)
            name = port

        self.thread.line_received.connect(self.on_line)
        self.thread.connection_error.connect(self.on_error)
        self.thread.start()

        self.btn_connect.setText("DISCONNECT")
        self.btn_connect.setStyleSheet(
            "background-color: #ff8c00; color: #fff; padding: 8px; "
            "font-weight: bold; border: none; border-radius: 4px;")
        self.combo_ports.setEnabled(False)
        self.lbl_status.setText(f"{name}")
        self.lbl_status.setStyleSheet(f"color: {FG_MUTED}; font-size: 13px; margin-left: 20px;")

    def disconnect_port(self):
        if self.thread is not None:
            self.thread.stop()
            self.thread.wait(2000)
            self.thread = None
        self.btn_connect.setText("CONNECT")
        self.btn_connect.setStyleSheet(
            "background-color: #0055a4; color: #fff; padding: 8px; "
            "font-weight: bold; border: none; border-radius: 4px;")
        self.combo_ports.setEnabled(True)
        self.lbl_status.setText("Not connected")
        self.lbl_status.setStyleSheet(f"color: {FG_MUTED}; font-size: 13px; margin-left: 20px;")

    def on_error(self, message):
        self.lbl_status.setText(f"Serial error: {message}")
        self.lbl_status.setStyleSheet(f"color: {ALERT}; font-size: 13px; margin-left: 20px;")
        self.disconnect_port()

    # ─────────────────────────────────────────────────────────────────────────
    # INCOMING DATA
    # ─────────────────────────────────────────────────────────────────────────

    def on_line(self, text):
        stripped = text.rstrip("\r\n")
        self.raw_lines.append(stripped)
        if self.chk_raw.isChecked() and stripped:
            self.append_text(stripped)

        report = self.parser.feed(text)
        if report is not None:
            self.reports.append(report)
            self.current = report
            if not self.chk_raw.isChecked():
                self.show_report(report)
            self.plot_report(report)

    def append_text(self, text):
        self.txt_report.appendPlainText(text)
        bar = self.txt_report.verticalScrollBar()
        bar.setValue(bar.maximum())

    def show_report(self, report):
        colour = FG_MUTED if report.faults() == "0" else ALERT
        self.append_text("")
        self.append_text("─" * 52)
        self.append_text(report.as_text())
        if report.faults() != "0":
            self.append_text("  *** TIMING FAULTS REPORTED — CHECK THE SESSION ***")
        self.append_text("─" * 52)
        self.lbl_status.setStyleSheet(f"color: {colour}; font-size: 13px; margin-left: 20px;")

    def refresh_text(self):
        """Switch the left pane between the reports and the raw log."""
        self.txt_report.clear()
        if self.chk_raw.isChecked():
            for line in self.raw_lines[-5000:]:
                if line:
                    self.append_text(line)
        else:
            for report in self.reports:
                self.show_report(report)

    # ─────────────────────────────────────────────────────────────────────────
    # PLOTS
    # ─────────────────────────────────────────────────────────────────────────

    def clear_plots(self, message):
        self.figure.clear()
        ax = self.figure.add_subplot(111)
        ax.set_facecolor(BG_PLOT)
        ax.text(0.5, 0.5, message, ha="center", va="center", color=FG_MUTED, fontsize=11)
        ax.axis("off")
        self.canvas.draw_idle()

    def style_axis(self, ax, xlabel, ylabel, title):
        ax.set_facecolor(BG_PLOT)
        ax.set_title(title, color=ACCENT, fontsize=10, fontweight="bold", loc="left")
        ax.set_xlabel(xlabel, color=FG_TEXT, fontsize=9)
        ax.set_ylabel(ylabel, color=FG_TEXT, fontsize=9)
        ax.tick_params(colors=FG_MUTED, labelsize=8)
        ax.grid(True, color=GRID, linewidth=0.6, alpha=0.6)
        ax.set_axisbelow(True)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            ax.spines[side].set_color(GRID)

    def plot_report(self, report):
        """
        Top panel: the distribution on a linear axis — one bar for Continuous,
        two for Burst, the full histogram for NPS.

        Bottom panel: the same data in log-log. A power law of unitary exponent,
        the form reported for NPS, is a straight line of slope -1 here, so the
        fit and the 1/x reference are drawn for NPS only.
        """
        values, source = report.interval_data()
        if not values:
            self.clear_plots("This report carries no interval data.")
            self.current_values, self.current_source = None, None
            self.btn_txt.setEnabled(False)
            self.btn_png.setEnabled(False)
            return

        self.current_values, self.current_source = values, source

        a = np.asarray(values, dtype=float)
        a = a[a > 0]
        unique = np.unique(a)
        discrete = unique.size <= 4          # Continuous and Burst

        self.figure.clear()
        self.figure.patch.set_facecolor(BG_PANEL)
        ax1 = self.figure.add_subplot(211)
        ax2 = self.figure.add_subplot(212)

        mode = report.fields.get("Type", "?")

        # ── Top panel: linear distribution ───────────────────────────────────
        if discrete:
            counts = [int((a == v).sum()) for v in unique]
            span = max(unique.max() - unique.min(), unique.max() * 0.2)
            ax1.bar(unique, counts, width=span * 0.06, color=DATA,
                    edgecolor=BG_PLOT, linewidth=0.5)
            for v, c in zip(unique, counts):
                ax1.annotate(f"{v:.0f} ms\nn = {c}", (v, c), textcoords="offset points",
                             xytext=(0, 6), ha="center", color=FG_TEXT, fontsize=8)
            ax1.set_xlim(max(unique.min() * 0.5, 0), unique.max() * 1.25)
            ax1.set_ylim(0, max(counts) * 1.35)
        else:
            ax1.hist(a, bins=np.linspace(a.min(), a.max(), 40),
                     color=DATA, edgecolor=BG_PLOT, linewidth=0.5)
        self.style_axis(ax1, "Inter-stimulus interval (ms)", "Count",
                        f"Interval distribution — {mode}")

        # ── Bottom panel: log-log ────────────────────────────────────────────
        if discrete:
            counts = np.array([int((a == v).sum()) for v in unique], dtype=float)
            ax2.loglog(unique, counts / counts.sum(), "o", color=DATA, markersize=7,
                       markeredgecolor=BG_PLOT, markeredgewidth=0.6,
                       label="interval values")
            ax2.set_xlim(unique.min() * 0.5, unique.max() * 2.0)
            ax2.set_ylim(min(counts.min() / counts.sum(), 0.05) * 0.4, 2.0)
            note = "deterministic: no distribution to fit"
            ax2.annotate(note, (0.98, 0.9), xycoords="axes fraction", ha="right",
                         color=FG_MUTED, fontsize=8)
        else:
            lbins = np.logspace(np.log10(a.min()), np.log10(a.max()), 25)
            dens, edges = np.histogram(a, bins=lbins, density=True)
            centres = np.sqrt(edges[:-1] * edges[1:])
            keep = dens > 0
            ax2.loglog(centres[keep], dens[keep], "o", color=DATA, markersize=5,
                       markeredgecolor=BG_PLOT, markeredgewidth=0.6, label="measured")
            if keep.sum() >= 3:
                x, y = np.log10(centres[keep]), np.log10(dens[keep])
                slope, intercept = np.polyfit(x, y, 1)
                resid = y - (slope * x + intercept)
                ss_tot = float(((y - y.mean()) ** 2).sum())
                r2 = 1 - float((resid ** 2).sum()) / ss_tot if ss_tot else float("nan")
                xr = np.array([a.min(), a.max()])
                ax2.loglog(xr, 10 ** intercept * xr ** slope, "-", color=ACCENT,
                           linewidth=1.8, label=f"fit: slope {slope:.2f}, R² {r2:.3f}")
                k = 10 ** intercept * centres[keep][0] ** (slope + 1)
                ax2.loglog(xr, k / xr, "--", color=FG_MUTED, linewidth=1.2,
                           label="reference 1/x")

        self.style_axis(ax2, "Inter-stimulus interval (ms)",
                        "Fraction of intervals" if discrete else "Probability density",
                        "Log-log")
        leg = ax2.legend(frameon=False, fontsize=8, loc="lower left")
        for text in leg.get_texts():
            text.set_color(FG_TEXT)

        cv = a.std(ddof=1) / a.mean() if a.size > 1 else 0.0
        caption = (f"n = {a.size}    min {a.min():.0f} ms    max {a.max():.0f} ms    "
                   f"mean {a.mean():.1f} ms    CV {cv:.2f}    ·    intervals {source}")
        self.figure.text(0.01, 0.01, caption, color=FG_MUTED, fontsize=8, ha="left")
        self.figure.tight_layout(rect=[0, 0.03, 1, 1])
        self.canvas.draw_idle()

        self.btn_txt.setEnabled(True)
        self.btn_png.setEnabled(True)

    # ─────────────────────────────────────────────────────────────────────────
    # EXPORTS
    # ─────────────────────────────────────────────────────────────────────────

    def save_txt(self):
        """The parameters as a comment header, then one interval per line."""
        if self.current is None or not getattr(self, "current_values", None):
            return
        stamp = time.strftime("%Y%m%d_%H%M%S")
        path, _ = QFileDialog.getSaveFileName(
            self, "Save intervals", f"GilaMonster_{stamp}_iti.txt", "Text files (*.txt)")
        if not path:
            return
        with open(path, "w") as fh:
            for key, value in self.current.fields.items():
                fh.write(f"# {key}: {value}\n")
            fh.write(f"# Intervals {self.current_source}\n")
            fh.write("# ITI_ms\n")
            for value in self.current_values:
                fh.write(f"{value:g}\n")
        self.lbl_status.setText(f"Saved {os.path.basename(path)}")

    def clear_sessions(self):
        """Discard every report received so far and reset both panes."""
        if not self.reports and not self.raw_lines:
            return
        answer = QMessageBox.question(
            self, "Clear sessions",
            f"Discard {len(self.reports)} report(s) and the serial log?\n"
            "Anything not saved is lost.",
            QMessageBox.Yes | QMessageBox.No, QMessageBox.No)
        if answer != QMessageBox.Yes:
            return

        self.reports = []
        self.raw_lines = []
        self.current = None
        self.current_values = None
        self.current_source = None
        self.parser = StreamParser()
        self.txt_report.clear()
        self.clear_plots("Waiting for a report.")
        self.btn_txt.setEnabled(False)
        self.btn_png.setEnabled(False)
        self.lbl_status.setText("Sessions cleared")
        self.lbl_status.setStyleSheet(f"color: {FG_MUTED}; font-size: 13px; margin-left: 20px;")

    def save_log(self):
        """The left pane as displayed: the formatted reports, or the raw log."""
        text = self.txt_report.toPlainText()
        if not text.strip():
            self.lbl_status.setText("Nothing to save yet")
            return
        stamp = time.strftime("%Y%m%d_%H%M%S")
        path, _ = QFileDialog.getSaveFileName(
            self, "Save log", f"GilaMonster_{stamp}_log.txt", "Text files (*.txt)")
        if not path:
            return
        with open(path, "w") as fh:
            fh.write(text + "\n")
        self.lbl_status.setText(f"Saved {os.path.basename(path)}")

    def save_png(self):
        if self.current is None or not getattr(self, "current_values", None):
            return
        stamp = time.strftime("%Y%m%d_%H%M%S")
        path, _ = QFileDialog.getSaveFileName(
            self, "Save plots", f"GilaMonster_{stamp}_iti.png", "PNG image (*.png)")
        if not path:
            return
        self.figure.savefig(path, dpi=200, facecolor=BG_PANEL)
        self.lbl_status.setText(f"Saved {os.path.basename(path)}")

    # ─────────────────────────────────────────────────────────────────────────
    # APPLICATION SHUTDOWN
    # ─────────────────────────────────────────────────────────────────────────

    def closeEvent(self, event):
        if self.thread:
            self.thread.stop()
            self.thread.wait(2000)
        event.accept()


# ════════════════════════════════════════════════════════════════════════════
# ENTRY POINT
# ════════════════════════════════════════════════════════════════════════════

if __name__ == "__main__":
    demo = None
    if "--demo" in sys.argv:
        i = sys.argv.index("--demo")
        demo = sys.argv[i + 1] if len(sys.argv) > i + 1 else "sample_log.txt"

    # Spyder / IPython: reuse existing QApplication to avoid RuntimeError.
    app = QApplication.instance()
    if app is None:
        app = QApplication(sys.argv)
    window = GilaMonitor(demo_file=demo)
    window.show()
    app.exec_()
