"""The installer window: MainWindow.xaml / MainWindow.xaml.cs of the WPF builder in Qt, the same 960x600 window, pages and logic. It
opens with the hookshot in the middle while it looks for the game files (and, at the first start or after an update, gets the build
tools), then walks through Game files > Build > Play. The game's update offer opens it on its update screen, and the game folder's
launcher (Hookshot.lnk, --launch <folder>) on the launcher page."""
import ctypes
import math
import os
import re
import sys
import time
import winreg

from PySide6.QtCore import (QEasingCurve, QObject, QPointF, QPropertyAnimation, QRectF, QSize, Qt, QTimer, QUrl, Signal)
from PySide6.QtGui import (QColor, QDesktopServices, QGuiApplication, QIcon, QLinearGradient, QPainter, QPainterPath, QPen,
                           QPixmap, QRadialGradient)
from PySide6.QtWidgets import (QAbstractButton, QApplication, QFileDialog, QFrame, QGraphicsOpacityEffect, QGridLayout,
                               QHBoxLayout, QLabel, QMessageBox, QPlainTextEdit, QScrollArea, QSizePolicy, QStackedLayout,
                               QVBoxLayout, QWidget)

from . import core, paths, setup_parts, strings, updater
from .theme import (C, MONO, W_BOLD, W_MEDIUM, W_SEMIBOLD, Bar, Button, Card, Chip, Choice, PathEdit, Ring, Tick, argb, css,
                    faint, font, glow, heading, label, note, rounded, section, set_color, sub, tick_path)

NEEDED_BYTES = 45 << 30   # (the game data 24.4 GB, the shader caches ~8 GB, the game 0.4 GB, the build files ~5 GB at their peak)

STAGES = [("download", "", "Download the update"), ("read", "Read your game files", "Read the game files kept in the folder"),
          ("lift", "Translate the game to PC code", "Translate the game to PC code"), ("compile", "Compile", "Compile changed files"),
          ("finish", "Put the game together", "Swap in the new game"), ("shaders", "Prepare the shaders", "Prepare the shaders")]
WITHOUT_SHADERS = {"download": (0, 0.04), "read": (0.04, 0.04), "lift": (0.08, 0.14), "compile": (0.22, 0.74), "finish": (0.96, 0.04)}
# (the game's --prepare-shaders run, make_game.py's last step: minutes on some GPUs, the first start's wait moved here)
WITH_SHADERS = {"download": (0, 0.04), "read": (0.04, 0.04), "lift": (0.08, 0.14), "compile": (0.22, 0.68), "finish": (0.90, 0.02),
                "shaders": (0.92, 0.08)}
SHADERS_LATER = "--no-prepare-shaders"
NOT_PREPARED = re.compile(r"^the shaders were not prepared now \((.*)\): (.*)$")   # (make_game.py NOT_PREPARED)
FIRST_START = "the game prepares them at its first start"   # (make_game.py FIRST_START)


def shaders_note(m):
    """The shaders row's line for make_game's not-prepared line: left to the first start and why, or (the GPU driver stopped
    responding) why and what happens instead"""
    why, then = m.groups()
    return ("Left to the game's first start: %s" % why if then == FIRST_START
            else "Not prepared: %s. %s" % (why, then[:1].upper() + then[1:]))

DLSS_LICENSE = (
    "NVIDIA DLSS is NVIDIA's, not Hookshot's. With it ticked, the build downloads NVIDIA's DLSS SDK (release v310.9.16) from "
    "github.com/NVIDIA/DLSS, under NVIDIA's own license, the NVIDIA RTX SDKs license:\n\n"
    "https://github.com/NVIDIA/DLSS/blob/9981c6350dc58c0ce21f024fd65e31e851c31689/LICENSE.txt\n\n"
    "Its files stay on this PC (in the builder's ngx-sdk folder, and nvngx_dlss.dll and nvngx_dlssg.dll in the game "
    "folder's ngx folder). "
    "Hookshot never includes or shares them: never share a game folder built with them.\n\nDo you accept NVIDIA's license?")


class Poster(QObject):
    """Calls from worker threads run on the window's thread"""
    call = Signal(object, object)

    def __init__(self):
        super().__init__()
        self.call.connect(lambda fn, args: fn(*args))


def clock(secs):
    return "%d:%02d" % (int(secs) // 60, int(secs) % 60)


def span(secs):
    secs = int(secs)
    if secs >= 3600:
        return "%d h %d min" % (secs // 3600, secs % 3600 // 60)
    if secs >= 60:
        return "%d minutes" % (secs // 60)
    return "%d seconds" % secs


def time_left(seconds):
    if seconds < 45:
        return "Less than a minute left"
    if seconds < 90:
        return "About a minute left"
    m = int(round(seconds / 60))
    return "About %d h %d min left" % (m // 60, m % 60) if m >= 60 else "About %d minutes left" % m


# ================= small pieces drawn as the XAML draws them =================

class Splash(QWidget):
    """The start: the hookshot (178 wide) over its pulsing halo, bobbing; "Hookshot", the status, and the sweeping bar"""

    def __init__(self, hook):
        super().__init__()
        self.hook, self.t0, self.text, self.scale = hook, time.monotonic(), "Starting…", 1.0
        self.timer = QTimer(self, timeout=self.update)
        self.timer.start(16)

    def stop(self):
        self.timer.stop()

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHints(QPainter.RenderHint.Antialiasing | QPainter.RenderHint.SmoothPixmapTransform | QPainter.RenderHint.TextAntialiasing)
        t = time.monotonic() - self.t0
        ping = (1 - math.cos(math.pi * ((t / 1.2) % 2 if (t / 1.2) % 2 <= 1 else 2 - (t / 1.2) % 2))) / 2   # (sine in-out, auto-reverse)
        cx, cy = self.width() / 2, self.height() / 2
        p.translate(cx, cy)
        p.scale(self.scale, self.scale)
        top = -(260 + 16 + 27 + 6 + 20 + 16 + 3) / 2   # (the stack: 260 hook area, the title, the status, the bar)
        hc = QPointF(0, top + 130)
        r = (260 - 48) / 2 * (0.94 + 0.10 * ping)   # (the halo: Margin 24, scale 0.94..1.04, opacity 0.6..1)
        g = QRadialGradient(hc, r)
        g.setColorAt(0, argb("#663D63E6"))
        g.setColorAt(0.68, argb("#003D63E6"))
        g.setColorAt(1, argb("#003D63E6"))
        p.setOpacity(0.6 + 0.4 * ping)
        p.setBrush(g)
        p.setPen(Qt.PenStyle.NoPen)
        p.drawEllipse(hc, r, r)
        p.setOpacity(1)
        w = 178
        h = self.hook.height() * w / self.hook.width()
        p.save()
        p.translate(hc.x(), hc.y() - 6 * ping)   # (BobLift 0..-6, BobTurn -4..2)
        p.rotate(-4 + 6 * ping)
        shadow = QRectF(-w / 2, -h / 2 + 14, w, h)   # (DropShadow 24 blur, 14 down, 0.6: the image's own, softened)
        p.setOpacity(0.35)
        p.drawPixmap(shadow, self.shadow_pixmap(), QRectF(self.shadow_pixmap().rect()))
        p.setOpacity(1)
        p.drawPixmap(QRectF(-w / 2, -h / 2, w, h), self.hook, QRectF(self.hook.rect()))
        p.restore()
        y = top + 260 + 16
        p.setPen(C["Ink"])
        p.setFont(font(22, W_SEMIBOLD))
        p.drawText(QRectF(-200, y, 400, 28), Qt.AlignmentFlag.AlignCenter, "Hookshot")
        y += 28 + 6
        p.setPen(argb("#A6FFFFFF"))
        p.setFont(font(13))
        p.drawText(QRectF(-300, y, 600, 20), Qt.AlignmentFlag.AlignCenter, self.text)
        y += 20 + 16
        track = QRectF(-100, y, 200, 3)
        p.setClipPath(rounded(track, 1.5))
        p.fillPath(rounded(track, 1.5), argb("#14FFFFFF"))
        f = (t / 1.2) % 1
        f = (1 - math.cos(math.pi * f)) / 2
        x = -100 - 88 + 296 * f   # (Sweep -88..208, sine in-out, repeating)
        lg = QLinearGradient(x, 0, x + 80, 0)
        lg.setColorAt(0, argb("#007F9BFF"))
        lg.setColorAt(0.5, argb("#FF7F9BFF"))
        lg.setColorAt(1, argb("#007F9BFF"))
        p.fillRect(QRectF(x, y, 80, 3), lg)

    _shadow = None

    def shadow_pixmap(self):
        if Splash._shadow is None:
            img = self.hook.toImage().convertToFormat(self.hook.toImage().Format.Format_ARGB32_Premultiplied)
            sp = QPixmap(img.size())
            sp.fill(Qt.GlobalColor.transparent)
            q = QPainter(sp)
            q.drawImage(0, 0, img)
            q.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceIn)
            q.fillRect(sp.rect(), QColor(0, 0, 0))
            q.end()
            Splash._shadow = sp.scaled(sp.width() // 6, sp.height() // 6, Qt.AspectRatioMode.IgnoreAspectRatio,
                                       Qt.TransformationMode.SmoothTransformation)
        return Splash._shadow


class StepPill(QWidget):
    """The head's pill: Game files > Build > Play (done: a green tick disc; now: the blue pill with its glow)"""

    def __init__(self):
        super().__init__()
        self.on = 0
        self.setFixedHeight(40)
        self.names = ["Game files", "Build", "Play"]
        self._layout()

    def set(self, on):
        self.on = on
        self._layout()
        self.update()

    def _layout(self):
        self.items = []
        x = 4 + 1
        for i, n in enumerate(self.names):
            if i:
                x += 5 + 4 + 5   # (the chevron, 5 each side)
            fw = W_SEMIBOLD if i == self.on else W_MEDIUM
            from PySide6.QtGui import QFontMetricsF
            tw = QFontMetricsF(font(13, fw)).horizontalAdvance(n)
            done = i < self.on
            w = (4 + 22 + 8 + tw + 14) if done else (14 + tw + 14)
            self.items.append((x, w, n, done, fw))
            x += w
        self.setFixedWidth(int(x + 4 + 1 + 0.5))

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHints(QPainter.RenderHint.Antialiasing | QPainter.RenderHint.TextAntialiasing)
        r = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        p.fillPath(rounded(r, 20), argb("#0BFFFFFF"))
        p.setPen(QPen(argb("#17FFFFFF"), 1))
        p.drawPath(rounded(r, 20))
        for i, (x, w, n, done, fw) in enumerate(self.items):
            if i:
                cx = x - 5 - 2
                p.setPen(QPen(QColor(C["InkSoft"].red(), C["InkSoft"].green(), C["InkSoft"].blue(), int(255 * 0.45)), 1.6))
                path = QPainterPath(QPointF(cx - 2, 16))
                path.lineTo(cx + 2, 20)
                path.lineTo(cx - 2, 24)
                p.drawPath(path)
            pill = QRectF(x, 5, w, 30)
            if i == self.on:   # (LinearGradient #4B70F0 -> #3557D6 at 90 deg, and its glow)
                for k in range(8, 0, -1):
                    gc = QColor(0x3D, 0x63, 0xE6, int(0.6 * 255 / 8 / k * 2))
                    p.fillPath(rounded(pill.adjusted(-k, -k + 6, k, k + 6), 15 + k), gc)
                lg = QLinearGradient(pill.topLeft(), pill.bottomLeft())
                lg.setColorAt(0, argb("#4B70F0"))
                lg.setColorAt(1, argb("#3557D6"))
                p.fillPath(rounded(pill, 15), lg)
            tx = x + 14
            if done:
                d = QRectF(x + 4, 9, 22, 22)
                p.setBrush(C["GoSoft"])
                p.setPen(Qt.PenStyle.NoPen)
                p.drawEllipse(d)
                p.setPen(QPen(C["Go"], 1.8))
                p.setBrush(Qt.BrushStyle.NoBrush)
                p.drawPath(tick_path(d.center().x(), d.center().y() + 0.25))
                tx = x + 4 + 22 + 8
            p.setFont(font(13, fw))
            p.setPen(C["White"] if i == self.on else C["InkSoft"] if done else C["InkFaint"])
            p.drawText(QRectF(tx, 5, w, 30), Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft, n)


class Mark(QWidget):
    """A build step's mark: waiting (a ring), now (the number on Brand with its halo), done (a tick on GoSoft), stopped (! on BadSoft)"""

    def __init__(self, n):
        super().__init__()
        self.n, self.state = n, "wait"
        self.setFixedSize(34, 34)

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHints(QPainter.RenderHint.Antialiasing | QPainter.RenderHint.TextAntialiasing)
        c = QPointF(11, 12)
        s = self.state
        if s == "now":
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(C["BrandSoft"])
            p.drawEllipse(c, 16, 16)
            p.setBrush(C["Brand"])
            p.drawEllipse(c, 11, 11)
            p.setPen(C["White"])
            p.setFont(font(12, W_SEMIBOLD))
            p.drawText(QRectF(c.x() - 11, c.y() - 11, 22, 22), Qt.AlignmentFlag.AlignCenter, str(self.n))
        elif s == "done":
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(C["GoSoft"])
            p.drawEllipse(c, 11, 11)
            p.setPen(QPen(C["Go"], 1.8))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawPath(tick_path(c.x(), c.y() + 0.25))
        elif s == "stopped":
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(C["BadSoft"])
            p.drawEllipse(c, 11, 11)
            p.setPen(C["Bad"])
            p.setFont(font(12, W_SEMIBOLD))
            p.drawText(QRectF(c.x() - 11, c.y() - 11, 22, 22), Qt.AlignmentFlag.AlignCenter, "!")
        else:
            p.setPen(QPen(C["GlassLineStrong"], 1.5))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawEllipse(c, 10.25, 10.25)
            if s == "skipped":
                p.setPen(C["InkFaint"])
                p.setFont(font(12, W_SEMIBOLD))
                p.drawText(QRectF(c.x() - 11, c.y() - 11, 22, 22), Qt.AlignmentFlag.AlignCenter, "–")


class StepRow(QWidget):
    """One step of the build: its mark (done, now, waiting), its name, a line under it and its time"""

    def __init__(self, n, text, last):
        super().__init__()
        self.running, self.start = False, 0.0
        self.last = last
        g = QGridLayout(self)
        g.setContentsMargins(0, 10, 0, 10)
        g.setHorizontalSpacing(0)
        g.setVerticalSpacing(0)
        self.mark = Mark(n)
        g.addWidget(self.mark, 0, 0, 2, 1, Qt.AlignmentFlag.AlignTop)
        self.name = label(text, 14, W_SEMIBOLD)
        self.small = label("", 12)
        self.small.hide()
        g.addWidget(self.name, 0, 1)
        g.addWidget(self.small, 1, 1)
        self.time = label("—", 12, mono=True)
        g.addWidget(self.time, 0, 2, Qt.AlignmentFlag.AlignTop)
        g.setColumnStretch(1, 1)
        self.wait()

    def paintEvent(self, e):
        if self.last:
            return
        p = QPainter(self)   # (the link down to the next mark: 1 wide at 10.5, from 28 below the top to 19 past the bottom)
        p.fillRect(QRectF(10.5, 10 + 28, 1, self.height() - 38 + 10), C["GlassLine"])

    def small_text(self, t):
        self.small.setText(t)
        self.small.show()

    def wait(self):
        self.mark.state = "wait"
        self.mark.update()
        self.name.setFont(font(14, W_MEDIUM))
        set_color(self.name, "InkFaint")
        set_color(self.small, "InkFaint")
        set_color(self.time, "InkFaint")

    def begin(self):
        self.running, self.start = True, time.monotonic()
        self.mark.state = "now"
        self.mark.update()
        self.name.setFont(font(14, W_SEMIBOLD))
        set_color(self.name, "Ink")
        set_color(self.time, "Ink")
        self.tick()

    def tick(self):
        if self.running:
            self.time.setText(clock(time.monotonic() - self.start))

    def done(self):
        if self.running:
            self.tick()
        self.running = False
        self.mark.state = "done"
        self.mark.update()
        self.name.setFont(font(14, W_SEMIBOLD))
        set_color(self.name, "Ink")
        set_color(self.time, "InkFaint")

    def stopped(self):
        self.running = False
        self.mark.state = "stopped"
        self.mark.update()

    def skipped(self):
        """(not done: left to later, the shaders' step when they were not prepared now)"""
        if self.running:
            self.tick()
        self.running = False
        self.mark.state = "skipped"
        self.mark.update()
        self.name.setFont(font(14, W_MEDIUM))
        set_color(self.name, "InkFaint")
        set_color(self.time, "InkFaint")


class DumpRow(QAbstractButton):
    """A found .nup: the whole row is the choice (RadioButton DumpRow: 16/13 padding, the ring and its Brand dot, #08FFFFFF hover)"""

    def __init__(self, dump, chip, why=None):
        super().__init__()
        self.setCheckable(True)
        self.setAutoExclusive(True)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setToolTip(dump.path)
        self.hover = False
        lay = QHBoxLayout(self)
        lay.setContentsMargins(16 + 18 + 14, 13, 16, 13)
        texts = QVBoxLayout()
        texts.setSpacing(0)
        name = label(os.path.basename(dump.path), 14, W_SEMIBOLD)
        name.setTextFormat(Qt.TextFormat.PlainText)
        name.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        texts.addWidget(name)
        texts.addWidget(faint("%s · %s · %s" % (dump.where, core.size_text(dump.size), core.ago(dump.modified))))
        if why:
            w = faint(why)
            w.setWordWrap(True)
            set_color(w, "Bad")
            w.setContentsMargins(0, 4, 0, 0)
            texts.addWidget(w)
        lay.addLayout(texts, 1)
        lay.addSpacing(12)
        lay.addWidget(chip, 0, Qt.AlignmentFlag.AlignVCenter)

    def enterEvent(self, e):
        self.hover = True
        self.update()

    def leaveEvent(self, e):
        self.hover = False
        self.update()

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        if self.hover:
            p.fillRect(self.rect(), argb("#08FFFFFF"))
        c = QPointF(16 + 9, self.height() / 2)
        p.setPen(QPen(C["Brand"] if self.isChecked() else C["InkFaint"], 2))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawEllipse(c, 8, 8)
        if self.isChecked():
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(C["Brand"])
            p.drawEllipse(c, 4, 4)


class Hline(QWidget):
    def __init__(self):
        super().__init__()
        self.setFixedHeight(1)

    def paintEvent(self, e):
        QPainter(self).fillRect(self.rect(), C["GlassLine"])


class Glyph(QWidget):
    """The small round badges: the "i" before the tools note, the "!" of a problem, the ready page's tick"""

    def __init__(self, kind):
        super().__init__()
        self.kind = kind
        self.setFixedSize({"info": QSize(16, 16), "bad": QSize(28, 28), "ready": QSize(124, 124)}[kind])

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHints(QPainter.RenderHint.Antialiasing | QPainter.RenderHint.TextAntialiasing)
        r = QRectF(self.rect())
        if self.kind == "info":
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(C["BrandSoft"])
            p.drawEllipse(r)
            p.setPen(argb("#B9C8FF"))
            p.setFont(font(10, W_BOLD))
            p.drawText(r, Qt.AlignmentFlag.AlignCenter, "i")
        elif self.kind == "bad":
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(C["BadSoft"])
            p.drawEllipse(r)
            p.setPen(C["Bad"])
            p.setFont(font(14, W_BOLD))
            p.drawText(r, Qt.AlignmentFlag.AlignCenter, "!")
        else:   # (84 wide: GoSoft with its 60-blur green glow, a ring of 10 at -10, the tick)
            c = r.center()
            for k in range(30, 0, -2):
                p.setPen(Qt.PenStyle.NoPen)
                p.setBrush(QColor(0x5F, 0xB0, 0x8A, int(255 * 0.55 * 0.06 * (1 - k / 30))))
                p.drawEllipse(c, 42 + k, 42 + k)
            p.setBrush(C["GoSoft"])
            p.drawEllipse(c, 42, 42)
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.setPen(QPen(argb("#125FB08A"), 10))
            p.drawEllipse(c, 47, 47)
            pen = QPen(C["Go"], 4)
            pen.setCapStyle(Qt.PenCapStyle.RoundCap)
            pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
            p.setPen(pen)
            path = QPainterPath(QPointF(c.x() - 14, c.y()))
            path.lineTo(c.x() - 4, c.y() + 10)
            path.lineTo(c.x() + 14, c.y() - 10)
            p.drawPath(path)


class LogBox(QPlainTextEdit):
    """The details: mono 11 in InkSoft on #33000000, GlassLine border, 10 padding, no wrapping"""

    def __init__(self, border=True):
        super().__init__()
        self.setReadOnly(True)
        self.setFont(font(11, family=MONO))
        self.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        self.setStyleSheet("QPlainTextEdit { color: %s; background: rgba(0,0,0,51); border: %s; border-radius: 0; padding: 10px; }"
                           % (css(C["InkSoft"]), "1px solid " + css(C["GlassLine"]) if border else "none") + SCROLLBARS)


SCROLLBARS = ("QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }"
              "QScrollBar::handle:vertical { background: rgba(255,255,255,40); border-radius: 3px; min-height: 24px; }"
              "QScrollBar::handle:vertical:hover { background: rgba(255,255,255,70); }"
              "QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }"
              "QScrollBar::handle:horizontal { background: rgba(255,255,255,40); border-radius: 3px; min-width: 24px; }"
              "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }"
              "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }")


def scroll(widget, max_h=None):
    s = QScrollArea()
    s.setWidget(widget)
    s.setWidgetResizable(True)
    s.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
    s.setFrameShape(QFrame.Shape.NoFrame)
    s.setStyleSheet("QScrollArea { background: transparent; } QScrollArea > QWidget > QWidget { background: transparent; }" + SCROLLBARS)
    s.viewport().setAutoFillBackground(False)
    widget.setAutoFillBackground(False)
    if max_h:
        s.setMaximumHeight(max_h)
    return s


def hbox(*items, spacing=0, margins=(0, 0, 0, 0)):
    l = QHBoxLayout()
    l.setSpacing(spacing)
    l.setContentsMargins(*margins)
    for it in items:
        if it is None:
            l.addStretch(1)
        elif isinstance(it, int):
            l.addSpacing(it)
        elif isinstance(it, QWidget):
            l.addWidget(it)
        else:
            l.addLayout(it)
    return l


def vbox(*items, spacing=0, margins=(0, 0, 0, 0)):
    l = QVBoxLayout()
    l.setSpacing(spacing)
    l.setContentsMargins(*margins)
    for it in items:
        if it is None:
            l.addStretch(1)
        elif isinstance(it, int):
            l.addSpacing(it)
        elif isinstance(it, QWidget):
            l.addWidget(it)
        else:
            l.addLayout(it)
    return l


def holder(layout):
    w = QWidget()
    w.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
    w.setLayout(layout)
    return w


# ================= the window =================

class Window(QWidget):
    def __init__(self, args):
        super().__init__()
        self.args = args
        self.poster = Poster()
        self.lang = strings.default_language()
        self.glass = False
        self.download = None
        self.runner = None
        self.rows = {}
        self.checks = {}
        self.dumps = []
        self.log_lines = []
        self.current = None
        self.finished = ""
        self.chosen_nup = self.romfs = self.exefs = None
        self.folders_note = None
        self.updating = self.checking = self.source_picked = False
        # (the first page has its game files and saved choices: a start with --update or --launch goes past that. back)
        self.files_ready = False
        self.kept_folder = ""
        self.compiled = 0.0
        self.files_done = self.files_of = 0
        self.files_changed, self.files_compiled = -1, 0
        self.compile_eta, self.this_compile, self.previous_total = -1.0, 0, -1
        self.last_failure = ""
        self.mode = "build"
        self.latest_tag = self.update_target = None
        self.launch_folder = ""
        self.problem_is_tools = False
        self.elapsed0 = self.compile0 = None
        self.prepare_now, self.shaders_began, self.shaders_later, self.built_in = True, False, None, None
        self.weight = WITHOUT_SHADERS
        self.setWindowTitle("Hookshot")
        self.setWindowIcon(QIcon(os.path.join(paths.ASSETS, "hookshot.ico")))
        self.setFixedSize(960, 600)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAcceptDrops(True)
        self.hook = QPixmap(os.path.join(paths.ASSETS, "hookshot.png"))
        self.clock = QTimer(self, interval=1000, timeout=self.tick)
        self._build_ui()
        self.out_path.setText(os.path.join(os.path.expanduser("~"), "Games", "Hookshot"))
        self.out_path.textChanged.connect(self.refresh)
        tools = core.Tools.find()
        self.tools_note.setText("Nothing else to install: Hookshot's own build tools are here, downloaded and checked by its setup."
                                if tools else "Hookshot's build tools are missing from its folder. Press Build and Hookshot downloads "
                                              "them again.")
        self.set_step(0)
        self.refresh()

    def post(self, fn, *args):
        self.poster.call.emit(fn, args)

    def T(self, key):
        return strings.text(self.lang, key)

    # ---------------- the native window: Windows 11's acrylic behind it (Glass.cs), our own caption ----------------

    def showEvent(self, e):
        super().showEvent(e)
        if not getattr(self, "_native", False):
            self._native = True
            self.glass = apply_glass(int(self.winId()))
            self.update()
            QTimer.singleShot(0, self.start)

    def nativeEvent(self, kind, message):
        if kind == b"windows_generic_MSG":
            msg = ctypes.wintypes.MSG.from_address(int(message))
            if msg.message == 0x0083 and msg.wParam:   # WM_NCCALCSIZE: the whole window is the client area (no title bar)
                return True, 0
            if msg.message == 0x0084:   # WM_NCHITTEST: the top 64 px (but the buttons in it) moves the window
                x, y = ctypes.c_short(msg.lParam & 0xFFFF).value, ctypes.c_short((msg.lParam >> 16) & 0xFFFF).value
                pos = self.mapFromGlobal(self.screen_point(x, y))
                if 0 <= pos.y() < 64:
                    w = self.childAt(pos)
                    if w is None or not isinstance(w, (QAbstractButton,)) and w.parent() is not None and not isinstance(
                            w.parent(), QAbstractButton):
                        return True, 2   # HTCAPTION
        return super().nativeEvent(kind, message)

    def screen_point(self, x, y):
        from PySide6.QtCore import QPoint
        dpr = self.devicePixelRatioF()
        return QPoint(int(x / dpr), int(y / dpr))

    def paintEvent(self, e):
        p = QPainter(self)
        r = QRectF(self.rect())
        if not self.glass:   # (Windows 10, a light Windows, or no acrylic: the plain Slate page)
            p.fillRect(r, C["Page"])
        lg = QLinearGradient(0, 0, 0, r.height())
        lg.setColorAt(0, argb("#401A1D24"))
        lg.setColorAt(1, argb("#3312141A"))
        p.fillRect(r, lg)

    # ---------------- the layout (MainWindow.xaml) ----------------

    def _build_ui(self):
        stack = QStackedLayout(self)
        stack.setStackingMode(QStackedLayout.StackingMode.StackAll)
        self.splash = Splash(self.hook)
        self.main = QWidget()
        self.main.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.main.hide()   # (shown as the splash fades: a fade of its own would hide the glows inside it, Qt draws one effect deep)
        self.main.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        stack.addWidget(self.main)
        stack.addWidget(self.splash)
        self.splash.raise_()

        # the head: the hookshot, and the steps (or the update's versions)
        head = QWidget(self.main)
        head.setGeometry(0, 0, 960, 64)
        icon = QLabel(head)
        icon.setPixmap(self.hook.scaledToWidth(56, Qt.TransformationMode.SmoothTransformation))
        icon.setScaledContents(True)
        icon.setGeometry(28, 32 - int(28 * self.hook.height() / self.hook.width() / 2), 28,
                         int(28 * self.hook.height() / self.hook.width()))
        glow(icon, "#000000", 6, 2, 0.5)
        title = label("Hookshot", 15, W_BOLD)
        title.setParent(head)
        title.move(28 + 28 + 10, 32 - title.sizeHint().height() // 2)
        self.version_chip = label("", 12, color="InkSoft")
        self.version_chip.setParent(head)
        self.version_chip.move(title.x() + title.sizeHint().width() + 12, 32 - 8)
        self.version_chip.hide()
        self.step_pill = StepPill()
        self.step_pill.setParent(head)
        self.step_pill.move((960 - self.step_pill.width()) // 2, 12)

        body = QWidget(self.main)
        body.setGeometry(64, 64 + 16, 960 - 128, 600 - 64 - 16 - 34)
        self.pages = QStackedLayout(body)
        self.files_page = self._files_page()
        self.build_page = self._build_page()
        self.ready_page = self._ready_page()
        self.launch_page = self._launch_page()
        self.problem_page = self._problem_page()
        for pg in (self.files_page, self.build_page, self.ready_page, self.launch_page, self.problem_page):
            self.pages.addWidget(pg)

        # the window's own buttons, over everything
        self.caption = QWidget(self)
        self.caption.setGeometry(960 - 6 - 92, 6, 92, 36)
        mn, cl = Button(kind="caption", icon="min"), Button(kind="caption", icon="close")
        mn.setToolTip("Minimize")
        cl.setToolTip("Close")
        mn.clicked.connect(self.showMinimized)
        cl.clicked.connect(self.close_window)
        self.caption.setLayout(hbox(mn, cl))
        self.caption.raise_()

    def _files_page(self):
        self.files_title, self.files_sub = heading("Build your game"), sub("Pick your game files and where the game goes. Hookshot does "
                                                                            "the rest, once.")
        self.dump_list = QVBoxLayout()
        self.dump_list.setSpacing(0)
        dump_holder = holder(self.dump_list)
        card = Card()
        self.dump_scroll = scroll(dump_holder, 128)
        card.setLayout(vbox(self.dump_scroll))
        browse = Button("Browse…", "link")
        browse.clicked.connect(self.browse_nup)
        other = Button("romfs and exefs folders", "link", color=C["InkSoft"])
        other.clicked.connect(self.browse_folders)
        drop = faint(" · or drop the file on this window · ", 13)
        links = hbox(note("Not listed?"), 8, browse, drop, other, None)
        self.out_path = PathEdit()
        change = Button("Change", "quiet")
        change.clicked.connect(self.browse_out)
        self.work_note = note("", 12)
        self.work_change = Button("Change", "link", size=12)
        self.work_change.clicked.connect(self.browse_work)
        self.work_reset = Button("Default", "link", size=12)
        self.work_reset.clicked.connect(lambda: self.change_work(None))
        left = vbox(section("INSTALL THE GAME TO"), hbox(self.out_path, 10, change), 8,
                    hbox(self.work_note, 12, self.work_change, 12, self.work_reset))
        self.language_box = Choice()
        for code, name, _t in strings.LANGUAGES:
            self.language_box.addItem(name)
        self.language_box.setCurrentIndex(strings.LANGUAGES.index(self.lang))
        self.language_box.currentIndexChanged.connect(self.language_changed)
        lang_col = holder(vbox(section("GAME LANGUAGE"), self.language_box))
        lang_col.setFixedWidth(200)
        row3 = hbox(left, 20)
        row3.addWidget(lang_col, 0, Qt.AlignmentFlag.AlignTop)
        self.tools_note = note("", 12)
        self.worker_box = Choice()
        for c in core.worker_choices():
            self.worker_box.addItem(str(c))
        self.worker_box.setProperty("focus_ring", True)
        self.worker_box.setToolTip("Automatic uses available memory and any existing worker override. More workers need more memory and "
                                   "may not be faster.")
        self.prepare_tick = Tick("Prepare shaders now", True)
        self.prepare_tick.setToolTip("Prepares the game's shaders at the end of the build, a few minutes (longer on some GPUs). Unticked, "
                                     "the game prepares them at its first start, behind a black window.")
        self.prepare_tick.clicked.connect(self.save_settings)
        work_col = holder(vbox(section("BUILD WORKERS"), self.worker_box, 12, self.prepare_tick))
        work_col.setFixedWidth(160)
        row4 = hbox(hbox(Glyph("info"), 8, self.tools_note), 20)
        row4.addWidget(work_col, 0, Qt.AlignmentFlag.AlignTop)
        self.dlss_tick = Tick("NVIDIA DLSS (downloads NVIDIA's SDK under NVIDIA's license)")
        self.dlss_tick.clicked.connect(self.dlss_changed)
        dnote = note("For NVIDIA RTX graphics cards. NVIDIA's files stay on this PC: never share a game folder built with them.", 12)
        dnote.setContentsMargins(26, 4, 0, 0)
        found = section("FOUND ON THIS PC")
        content = holder(vbox(self.files_title, self.files_sub, found, card, 10, links, row3, 12, row4, 14, self.dlss_tick, dnote, None))
        self.space_bar = Bar("Go", 160)
        self.space_text = note("", 12)
        self.space_text.setWordWrap(False)
        self.about_text = faint("About 45 min")
        self.build_button = Button("Build the game", "primary")
        self.build_button.setEnabled(False)
        self.build_button.clicked.connect(self.start_build)
        foot = hbox(self.space_bar, 10, self.space_text, None, self.about_text, 14, self.build_button)
        page = holder(vbox(scroll(content), 12, foot))
        return page

    def _build_page(self):
        self.ring = Ring()
        self.build_title, self.build_note = heading("Building your game"), sub("")
        self.update_bar = Bar("Brand")
        self.update_bar.setContentsMargins(0, 18, 0, 0)
        self.update_bar.hide()
        self.ring_box = holder(hbox(self.ring, 28))   # (collapsed with its gap in the update)
        top = hbox(self.ring_box, vbox(None, self.build_title, self.build_note, 18, self.update_bar, None))
        self.stage_list = QVBoxLayout()
        self.stage_list.setSpacing(0)
        self.step_card = Card()
        self.step_card.setLayout(vbox(self.stage_list, margins=(18, 6, 18, 6)))
        self.log_box = LogBox()
        self.log_box.hide()
        mid = vbox(self.step_card, self.log_box, None)
        self.show_log = Button(self.T("ShowDetails"), "toggle")
        self.show_log.setCheckable(True)
        self.show_log.toggled.connect(self.log_toggled)
        self.cancel_button = Button(self.T("Cancel"), "quiet")
        self.cancel_button.clicked.connect(self.cancel_build)
        return holder(vbox(top, 22, mid, 14, hbox(self.show_log, None, self.cancel_button)))

    def _ready_page(self):
        self.ready_title = heading("Your game is ready")
        self.ready_note = sub("")
        self.ready_note.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.ready_note.setWordWrap(False)
        self.play_button = Button(self.T("Play"), "go", icon="play")
        self.play_button.clicked.connect(self.play)
        self.open_button = Button(self.T("OpenFolder"), "quiet")
        self.open_button.setFixedHeight(50)
        self.open_button.fixed_h = 50
        self.open_button.clicked.connect(lambda: core.open_folder(self.finished))
        self.desktop_tick = Tick("Desktop shortcut", True)
        self.start_tick = Tick("Start menu entry", True)
        self.desktop_tick.clicked.connect(self.shortcuts_changed)
        self.start_tick.clicked.connect(self.shortcuts_changed)
        self.storage_note = note("")
        self.storage_note.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.storage_note.setMaximumWidth(560)
        self.free_button = Button("Free up space", "link")
        self.free_button.clicked.connect(self.free_space)
        show = Button("Show folder", "link")
        show.clicked.connect(lambda: core.open_folder(core.work() if core.work_chosen() else paths.base()))
        self.storage_row = holder(vbox(hbox(None, self.storage_note, None), 8, hbox(None, self.free_button, 18, show, None)))
        self.storage_row.hide()
        col = vbox(None, hbox(None, Glyph("ready"), None), 24 - 20, hbox(None, self.ready_title, None), hbox(None, self.ready_note, None),
                   28, hbox(None, self.play_button, 12, self.open_button, None), 22,
                   hbox(None, self.desktop_tick, 18, self.start_tick, None), 26, self.storage_row, None)
        return holder(col)

    def _launch_page(self):
        z = section("THE LEGEND OF ZELDA")
        z.setContentsMargins(0, 0, 0, 0)
        t = heading("Ocarina of Time")
        t.setFont(font(34, W_BOLD))
        self.launch_version = sub("")
        self.launch_version.setContentsMargins(0, 8, 0, 0)
        self.launch_version.setWordWrap(False)
        self.launch_status = label("Checking for updates…", 13, color="InkSoft")
        self.launch_update = Button("Update", "primary")
        self.launch_update.setFixedHeight(50)
        self.launch_update.fixed_h, self.launch_update.padx = 50, 26
        self.launch_update.hide()
        self.launch_update.clicked.connect(lambda: updater.update_folder(self, self.launch_folder, self.update_target, False))
        self.launch_play = Button(self.T("Play"), "go", icon="play")
        self.launch_play.clicked.connect(self.launch_play_click)
        fold = Button("Open the game folder", "link", color=C["InkFaint"])
        fold.clicked.connect(lambda: core.open_folder(self.launch_folder))
        return holder(vbox(None, hbox(None, z, None), 4, hbox(None, t, None), hbox(None, self.launch_version, None), 16,
                           hbox(None, self.launch_status, None), 28, hbox(None, self.launch_update, 12, self.launch_play, None), 22,
                           hbox(None, fold, None), None))

    def _problem_page(self):
        self.problem_title, self.problem_sub = heading("The build stopped"), sub("Nothing was changed in your game folder.")
        self.problem_head = label("", 16, W_SEMIBOLD, wrap=True)
        self.problem_head.setMaximumWidth(740)
        self.problem_text = note("", 14)
        self.problem_steps = QVBoxLayout()
        self.problem_steps.setSpacing(0)
        self.problem_steps.setContentsMargins(0, 8, 0, 0)
        box = ProblemCard()
        head = hbox(Glyph("bad"), 12)
        head.addWidget(self.problem_head, 1)
        self.problem_scroll = scroll(holder(vbox(self.problem_text, self.problem_steps)), 190)
        box.setLayout(vbox(head, 10, self.problem_scroll, margins=(24, 22, 24, 22)))
        self.problem_details = Button("Show details", "link", color=C["InkSoft"])
        self.problem_details.clicked.connect(self.problem_show_details)
        self.copy_report = Button("Copy for a bug report", "link", color=C["InkSoft"])
        self.copy_report.clicked.connect(self.copy_for_report)
        self.back_button = Button(self.T("Back"), "quiet")
        self.back_button.clicked.connect(self.back)
        self.repair_button = Button("Repair", "primary")
        self.repair_button.clicked.connect(self.repair)
        foot = hbox(self.problem_details, faint("  ·  ", 13), self.copy_report, None, self.back_button, 10, self.repair_button)
        return holder(vbox(self.problem_title, self.problem_sub, 22, box, None, 14, foot))

    # ================= the start =================

    def start(self):
        if os.environ.get("HOOKSHOT_TEST_SHORTCUTS"):   # (development: both shortcuts made for it, the result beside the app)
            folder = os.environ["HOOKSHOT_TEST_SHORTCUTS"]
            core.shortcuts(folder, True, True)
            paths.write_small(os.path.join(paths.APP_DIR, "shortcut-test.txt"), "desktop %s start %s error %s" % (
                os.path.isfile(core.desktop_link()), os.path.isfile(core.start_link()), core.shortcut_error))
            QApplication.quit()
            return
        self.t_start = time.monotonic()
        a = self.args
        if a.update:
            self.splash.text = "Getting the update…"
            QTimer.singleShot(900, lambda: (self.show_main(), updater.update_folder(self, a.update, a.tag, a.fetched)))
            return
        if a.launch:
            self.splash.text = "Starting Hookshot…"
            QTimer.singleShot(700, lambda: (self.show_main(), self.show_launcher(a.launch.rstrip("\\/"))))
            return
        if a.repair or not core.Tools.find():   # (the first start, or Repair: the build tools, as the setup got them)
            self.splash.text = "Getting the build tools…"
            self.get_tools(a.repair)
            return
        self.find_files()

    def get_tools(self, repair):
        def status(s):
            self.splash.text = s

        def run():
            try:
                setup_parts.install(lambda s: self.post(status, s), tag=None, repair=repair)
                self.post(self.find_files)
            except setup_parts.SetupError as e:
                self.post(self.tools_failed, str(e))
        import threading
        threading.Thread(target=run, daemon=True).start()

    def tools_failed(self, why):
        self.show_main()
        self.show_problem(why, tools=False)
        self.problem_title.setText("The build tools could not be downloaded")
        self.problem_sub.setText("Nothing was changed on this PC.")
        self.repair_button.show()
        self.repair_button.setText("Try again")

    def find_files(self):
        self.splash.text = "Looking for your game files…"
        import threading

        def run():
            found = core.find_dumps()
            self.post(self.found_files, found)
        threading.Thread(target=run, daemon=True).start()

    def found_files(self, found):
        self.dumps.extend(found)
        n = len(found)
        self.splash.text = "No game files found yet" if n == 0 else "Found 1 game file" if n == 1 else "Found %d game files" % n
        wait = 1.7 - (time.monotonic() - self.t_start)
        QTimer.singleShot(max(0, int(wait * 1000)), self.found_go)

    def found_go(self):
        self.apply_settings()
        if self.kept_folder:   # (back from an update that stopped: its folder, the saved one is the last build's)
            self.out_path.setText(self.kept_folder)
        self.fill_dumps()
        self.files_ready = True
        self.show_main()
        if os.environ.get("HOOKSHOT_UI_PREVIEW"):
            self.preview(os.environ["HOOKSHOT_UI_PREVIEW"])
            return
        self.check_all()
        import threading

        def run():
            tag = updater.latest_tag()
            self.post(self.got_latest, tag)
        threading.Thread(target=run, daemon=True).start()

    def got_latest(self, tag):
        self.latest_tag = tag
        self.refresh_mode()

    def show_main(self):
        """The hookshot fades out and the first step fades in, in the same window"""
        if getattr(self, "_shown_main", False):
            return
        self._shown_main = True
        fx = QGraphicsOpacityEffect(self.splash)
        self.splash.setGraphicsEffect(fx)
        a = QPropertyAnimation(fx, b"opacity", self)
        a.setDuration(450)
        a.setStartValue(1.0)
        a.setEndValue(0.0)
        a.finished.connect(lambda: (self.splash.hide(), self.splash.stop()))
        a.start()
        self._splash_anim = a
        t0 = time.monotonic()

        def grow():
            f = min(1.0, (time.monotonic() - t0) / 0.5)
            self.splash.scale = 1 + 0.04 * f
            if f < 1:
                QTimer.singleShot(16, grow)
        grow()
        self.main.move(0, 10)
        lift = QPropertyAnimation(self.main, b"pos", self)
        lift.setDuration(600)
        lift.setStartValue(self.main.pos())
        from PySide6.QtCore import QPoint
        lift.setEndValue(QPoint(0, 0))
        lift.setEasingCurve(QEasingCurve.Type.OutCubic)
        self._lift_anim = lift
        QTimer.singleShot(150, lambda: (self.main.show(), self.splash.raise_(), self.caption.raise_(), lift.start()))
        self.main.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, False)

    def show_page(self, page):
        self.pages.setCurrentWidget(page)

    def set_step(self, on):
        self.step_pill.set(on)
        self.step_pill.move((960 - self.step_pill.width()) // 2, 12)
        self.step_pill.show()
        self.version_chip.hide()

    # ================= what the button does: build, update the game, play it, or get the newer builder =================

    def refresh_mode(self):
        if self.updating or self.runner:
            return
        out = self.out_path.text()
        game = core.game_version(out) if len(out) > 2 else None
        tools = core.Tools.find()
        builder = tools.version if tools else None
        self.update_target = self.latest_tag or builder
        if game and self.update_target and game != self.update_target and not self.source_picked:
            self.mode = "update_game"
        elif game and not self.source_picked:
            self.mode = "up_to_date"
        elif self.latest_tag and builder and builder != self.latest_tag:
            self.mode = "update_builder"
        else:
            self.mode = "build"
        if self.mode == "update_game":
            self.files_title.setText("Update available")
            self.files_sub.setText("Hookshot %s is available. Your game is %s: Update builds it again from the game files kept in its "
                                   "folder, compiling only what changed." % (self.update_target, game))
            self.build_button.setText("Update")
        elif self.mode == "up_to_date":
            self.files_title.setText("Your game is up to date")
            self.files_sub.setText("Your game is %s, as this builder. GitHub did not answer, so newer versions could not be checked." % game
                                   if not self.latest_tag else
                                   "%s is the latest version. Pick game files below only to build it again from scratch." % game)
            self.build_button.setText("Play")
        elif self.mode == "update_builder":
            self.files_title.setText("Build your game")
            self.files_sub.setText("Hookshot Builder %s is available (this one is %s): update it first, then build." % (self.latest_tag,
                                                                                                                     builder))
            self.build_button.setText("Update builder")
        else:
            self.files_title.setText("Build your game")
            self.files_sub.setText("Pick your game files and where the game goes. Hookshot does the rest, once.")
            self.build_button.setText("Build the game")
        self.build_button.setEnabled(self.mode != "build" or (self.source_ready() and len(out) > 2))
        self.about_text.setVisible(self.mode == "build")

    # ---- the launcher: the game folder's Hookshot.lnk starts this with --launch <folder> ----

    def show_launcher(self, folder):
        self.launch_folder = folder
        self.out_path.setText(folder)
        self.step_pill.hide()
        game = core.game_version(folder) or core.read_version(folder)
        self.launch_version.setText(("Hookshot %s" % game if game else "Hookshot") + " · " + folder)
        self.launch_play.setEnabled(os.path.isfile(core.game_exe(folder)))
        self.show_page(self.launch_page)
        import threading

        def run():
            tag = updater.latest_tag()
            self.post(self.launcher_latest, folder, game, tag)
        threading.Thread(target=run, daemon=True).start()

    def launcher_latest(self, folder, game, tag):
        self.latest_tag = tag
        tools = core.Tools.find()
        self.update_target = tag or (tools.version if tools else None)
        if not self.update_target:
            self.launch_status.setText("Could not check for updates (GitHub did not answer)")
        elif game and game != self.update_target and core.game_version(folder):
            self.launch_status.setText("Update available: %s" % self.update_target)
            self.launch_status.setFont(font(13, W_SEMIBOLD))
            set_color(self.launch_status, argb("#B9C8FF"))
            self.launch_update.show()
        elif game and game != self.update_target:
            self.launch_status.setText("%s is out: build this folder again with the Hookshot Builder to get it" % self.update_target)
        else:
            self.launch_status.setText("Up to date with this builder (GitHub did not answer)" if not tag else "Up to date")

    def launch_play_click(self):
        core.start_game(self.launch_folder)
        QApplication.quit()

    # ---- what the player chose last time ----

    def apply_settings(self):
        s = core.load_settings()
        if not s:
            return
        if s.get("Out"):
            self.out_path.setText(s["Out"])
        self.dlss_tick.setChecked(bool(s.get("Dlss")))
        self.dlss_license = bool(s.get("DlssLicense"))
        self.prepare_tick.setChecked(not s.get("PrepareLater"))
        r, x, n = s.get("Romfs"), s.get("Exefs"), s.get("Nup")
        if r and x and os.path.isdir(r) and os.path.isfile(os.path.join(x, "main")):
            self.romfs, self.exefs = r, x
            self.folders_note = "Using the folders %s and %s (as last time)" % (r, x)
        elif n and os.path.isfile(n):
            if not any(os.path.normcase(d.path) == os.path.normcase(n) for d in self.dumps):
                st = os.stat(n)
                self.dumps.insert(0, core.Dump(os.path.abspath(n), "Last time", st.st_size, st.st_mtime))
            self.chosen_nup = n

    def save_settings(self):
        core.save_settings({"Out": self.out_path.text(), "Nup": None if self.romfs else self.chosen_nup, "Romfs": self.romfs,
                            "Exefs": self.exefs, "Dlss": self.dlss_tick.isChecked(),
                            "DlssLicense": getattr(self, "dlss_license", False) or bool(core.load_settings().get("DlssLicense")),
                            "PrepareLater": not self.prepare_tick.isChecked()})

    def preview(self, what):
        """(development) HOOKSHOT_UI_PREVIEW=build|ready|problem|update|launch: one screen with made-up progress, nothing run"""
        if what == "build":
            self.make_rows(False)
            self.set_step(1)
            self.show_page(self.build_page)
            self.rows["read"].begin(), self.rows["read"].done(), self.rows["read"].small_text("Version checked")
            self.rows["lift"].begin(), self.rows["lift"].done()
            self.rows["compile"].begin(), self.rows["compile"].small_text("31 of 58 files")
            self.set_progress(0.54)
            self.build_note.setText("About 19 minutes left · your PC stays usable")
        elif what == "ready":
            self.finished = self.out_path.text()
            self.elapsed0 = time.monotonic() - 2700
            self.show_ready()
        elif what == "problem":
            self.show_problem("Files of the builder's own compiler are missing (clang.exe in tools\\llvm-mingw\\bin).", tools=True)
        elif what == "update":
            self.begin_update(self.out_path.text(), "honest-eta")
            self.rows["download"].done(), self.rows["download"].small_text("3 MB · build tools unchanged")
            self.rows["read"].begin(), self.rows["read"].done()
            self.rows["lift"].begin(), self.rows["lift"].done()
            self.rows["compile"].begin(), self.rows["compile"].small_text("7 of 516 files")
            self.set_progress(0.62)
            self.build_note.setText("About 3 minutes left. Your current game stays as it is until the new one is complete.")
        elif what == "launch":
            self.show_launcher(self.out_path.text())

    # ================= 1: game files =================

    def fill_dumps(self):
        while self.dump_list.count():
            it = self.dump_list.takeAt(0)
            if it.widget():
                it.widget().deleteLater()
        self.folders_label = None
        if self.romfs and self.exefs and self.folders_note:
            l = note(self.folders_note)
            l.setContentsMargins(16, 12, 16, 12)
            self.folders_label = l
            self.dump_list.addWidget(l)
        if not self.dumps:
            l = note("No .nup files in Downloads, Desktop, Documents or the top folders of your drives. Browse to yours, or drop it on "
                     "this window.")
            l.setContentsMargins(16, 14, 16, 14)
            self.dump_list.addWidget(l)
            self.fit_dumps()
            return
        for i, d in enumerate(self.dumps):
            if i:
                self.dump_list.addWidget(Hline())
            c = self.checks.get(d.path, "missing")
            why = c[1] if isinstance(c, tuple) and not c[0] and not c[1].startswith("This is build") else None
            row = DumpRow(d, self.chip(d.path), why)
            row.setChecked(self.chosen_nup is not None and os.path.normcase(d.path) == os.path.normcase(self.chosen_nup))
            row.clicked.connect(lambda _=False, p=d.path: self.choose(p))
            self.dump_list.addWidget(row)
        self.fit_dumps()

    def fit_dumps(self):
        """The list as tall as its rows, at most 128 (then it scrolls)"""
        def fit():
            self.dump_list.activate()
            h = sum(self.dump_list.itemAt(i).widget().sizeHint().height() for i in range(self.dump_list.count())
                    if self.dump_list.itemAt(i).widget() is not None and not self.dump_list.itemAt(i).widget().isHidden())
            self.dump_scroll.setFixedHeight(max(1, min(128, h + 1)))
            self.dump_scroll.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded if h > 128 else
                                                        Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        fit()
        QTimer.singleShot(0, fit)

    def chip(self, path):
        c = self.checks.get(path)
        if c is None:
            ch = Chip("Checking…", "InkFaint")
        elif c[0]:
            ch = Chip("✓ Supported version", "Go", "GoSoft")
        elif c[1].startswith("This is build"):
            ch = Chip("Not the supported version", "Warn")
        else:
            ch = Chip("Can't be read", "Bad")
        if c is not None and not c[0]:
            ch.setToolTip(c[1])
        return ch

    def check_all(self):
        """Each found file's version, one at a time; the first good one is chosen"""
        if self.checking:
            return
        tools = core.Tools.find()
        nxt = next((d for d in self.dumps if d.path not in self.checks), None) if tools else None
        if nxt is None:
            if self.chosen_nup is None and self.romfs is None and self.dumps and tools:
                self.chosen_nup = self.dumps[0].path
                self.fill_dumps()
            self.refresh()
            return
        self.checking = True
        self.checks[nxt.path] = None
        self.fill_dumps()
        import threading

        def run():
            r = core.check(tools, ["--check", "--nup", nxt.path])
            self.post(self.checked, nxt.path, r)
        threading.Thread(target=run, daemon=True).start()

    def checked(self, path, r):
        self.checking = False
        self.checks[path] = r
        if not r[0] and "antivirus" in r[1].lower():
            self.show_problem(r[1], tools=True)
            return
        if self.chosen_nup is None and self.romfs is None and r[0]:
            self.chosen_nup = path
        self.fill_dumps()
        self.refresh()
        self.check_all()

    def choose(self, path):
        self.source_picked = True
        self.chosen_nup = path
        self.romfs = self.exefs = None
        if self.folders_label is not None:
            self.folders_label.deleteLater()
            self.folders_label = None
        self.refresh()

    def add_dump(self, path, where):
        path = os.path.abspath(path)
        if not any(os.path.normcase(d.path) == os.path.normcase(path) for d in self.dumps):
            st = os.stat(path)
            self.dumps.insert(0, core.Dump(path, where, st.st_size, st.st_mtime))
        self.source_picked = True
        self.chosen_nup = path
        self.romfs = self.exefs = None
        self.fill_dumps()
        self.check_all()
        self.refresh()

    def source_ready(self):
        if self.romfs and self.exefs:
            return True
        c = self.checks.get(self.chosen_nup) if self.chosen_nup else None
        return bool(c and c[0])

    def refresh(self, *_):
        self.build_button.setEnabled(self.source_ready() and len(self.out_path.text()) > 2)
        self.show_work()
        root, freeb = core.free_space(self.out_path.text())
        if freeb is not None:
            self.space_bar.set(min(1.0, NEEDED_BYTES / max(1, freeb)), "Go" if freeb >= NEEDED_BYTES else "Bad")
            self.space_text.setText("Needs 45 GB · %s has %d GB free" % (root.rstrip("\\"), freeb >> 30))
            set_color(self.space_text, "InkSoft" if freeb >= NEEDED_BYTES else "Bad")
        else:
            self.space_text.setText("Needs 45 GB")
        self.refresh_mode()

    def dragEnterEvent(self, e):
        if e.mimeData().hasUrls() and not self.runner:
            e.acceptProposedAction()

    def dropEvent(self, e):
        for u in e.mimeData().urls():
            p = u.toLocalFile()
            if p.lower().endswith(".nup") and os.path.isfile(p):
                self.add_dump(p, "Dropped")
            elif os.path.isdir(os.path.join(p, "romfs")) and os.path.isdir(os.path.join(p, "exefs")):
                self.use_folders(os.path.join(p, "romfs"), os.path.join(p, "exefs"))

    def browse_nup(self):
        f, _ = QFileDialog.getOpenFileName(self, "Choose your game file", "", "Game file (*.nup)")
        if f:
            self.add_dump(os.path.normpath(f), "Chosen")

    def browse_folders(self):
        both = QFileDialog.getExistingDirectory(self, "Choose the folder with romfs and exefs (or the romfs folder)")
        if not both:
            return
        both = os.path.normpath(both)
        if os.path.isdir(os.path.join(both, "romfs")) and os.path.isdir(os.path.join(both, "exefs")):
            self.use_folders(os.path.join(both, "romfs"), os.path.join(both, "exefs"))
            return
        ex = QFileDialog.getExistingDirectory(self, "Now choose the exefs folder")
        if ex:
            self.use_folders(both, os.path.normpath(ex))

    def use_folders(self, r, x):
        if not os.path.isfile(os.path.join(x, "main")):
            QMessageBox.information(self, "Hookshot", "That exefs folder has no file named main.")
            return
        self.source_picked = True
        self.romfs, self.exefs, self.chosen_nup = r, x, None
        self.folders_note = "Using the folders %s and %s" % (r, x)
        self.fill_dumps()
        self.refresh()

    def browse_out(self):
        d = QFileDialog.getExistingDirectory(self, "Choose where the game goes")
        if d:
            self.out_path.setText(os.path.normpath(d))

    def browse_work(self):
        d = QFileDialog.getExistingDirectory(self, "Choose where Hookshot keeps its build files")
        if d:
            self.change_work(os.path.normpath(d))

    def change_work(self, picked):
        try:
            core.move_work(picked)
        except OSError as e:
            QMessageBox.information(self, "Hookshot", "Hookshot cannot keep its build files there: %s" % e)
        self.refresh()

    def show_work(self):
        w = core.work()
        self.work_reset.setVisible(core.work_chosen())
        self.work_note.setText(
            "Build files: about %s, kept in %s so that updates are quick (HOOKSHOT_KEEP_BUILD_FILES)." % (core.gb(core.BUILD_FILES_BYTES), w)
            if core.keep_build_files() else
            "Build files: about %s in %s while it builds; when the game is ready about 1 GB stays (the compiled code) so that updates "
            "compile only what changed." % (core.gb(core.BUILD_FILES_BYTES), w))
        set_color(self.work_note, "InkSoft")
        root, freeb = core.free_space(w)
        if freeb is not None and freeb < core.BUILD_FILES_BYTES:
            self.work_note.setText(self.work_note.text() + " %s has only %s free." % (root.rstrip("\\"), core.gb(freeb)))
            set_color(self.work_note, "Bad")

    def language_changed(self, i):
        self.lang = strings.LANGUAGES[i]
        self.cancel_button.setText(self.T("Cancel"))
        self.back_button.setText(self.T("Back"))
        self.open_button.setText(self.T("OpenFolder"))
        self.play_button.setText(self.T("Play"))
        self.show_log.setText(self.T("HideDetails" if self.show_log.isChecked() else "ShowDetails"))

    # ================= 2: the build =================

    def start_build(self):
        if self.mode == "update_game":
            self.save_settings()
            updater.update_folder(self, self.out_path.text(), self.update_target, False)
            return
        if self.mode == "up_to_date":
            self.finished = self.out_path.text()
            self.play()
            return
        if self.mode == "update_builder":
            self.save_settings()
            self.build_button.setEnabled(False)
            self.build_button.setText("Getting the new builder…")
            self.begin_update(self.out_path.text(), self.latest_tag)
            updater.run_latest_setup(self)
            return
        self.save_settings()
        tools = core.Tools.find()
        if not tools:
            self.show_problem("The build tools are not in Hookshot's folder.", tools=True)
            return
        if self.romfs and self.exefs:
            args = ["--romfs", self.romfs, "--exefs", self.exefs]
        elif self.chosen_nup:
            args = ["--nup", self.chosen_nup]
        else:
            return
        args += ["--out", self.out_path.text(), "--language", self.lang[0]]
        self.prepare_now = self.prepare_tick.isChecked()
        if not self.prepare_now:
            args.append(SHADERS_LATER)
        self.run(tools, args, dlss=self.dlss_tick.isChecked())

    def dlss_changed(self):
        if self.dlss_tick.isChecked():
            b = QMessageBox.question(self, "NVIDIA DLSS", DLSS_LICENSE, QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
            if b != QMessageBox.StandardButton.Yes:
                self.dlss_tick.setChecked(False)
            else:
                self.dlss_license = True   # (kept in the settings, DlssLicense: an update keeps DLSS by it, not the SDK's files)
        self.save_settings()   # (kept at once: ticked and closed, it is still ticked next time)

    def make_rows(self, update, shaders=False):
        while self.stage_list.count():
            it = self.stage_list.takeAt(0)
            if it.widget():
                it.widget().deleteLater()
        self.rows = {}
        self.weight = WITH_SHADERS if shaders else WITHOUT_SHADERS
        lst = [s for s in STAGES if (update or s[0] != "download") and (shaders or s[0] != "shaders")]
        for i, (sid, b, u) in enumerate(lst):
            row = StepRow(i + 1, u if update else b, i == len(lst) - 1)
            self.rows[sid] = row
            self.stage_list.addWidget(row)
        if update:
            self.rows["read"].small_text("Your game files are kept: the .nup isn't needed")

    def hard_drive_warning(self):
        """The game folder or the build files folder on a hard drive: the build works, but much more slowly than on an SSD.
        None when both are on an SSD (or their drive cannot be told, as on a network share)"""
        for what, folder in (("game folder", self.out_path.text()), ("build files folder", core.work())):
            if core.spinning(folder) is not True:
                continue
            root = os.path.splitdrive(os.path.abspath(folder))[0] or folder
            return ("The %s is on a hard drive (%s). An SSD builds the game much faster: build there if you can.\n\n"
                    "Choose OK to build here anyway, or Cancel to choose another folder." % (what, root))
        return None

    def run(self, tools, args, keep_rows=False, dlss=False):
        # (a warning, not a stop: the player may have no other folder with the room. An update keeps its own flow)
        slow = None if keep_rows else self.hard_drive_warning()
        if slow and QMessageBox.question(self, "Hookshot", slow, QMessageBox.StandardButton.Ok |
                                         QMessageBox.StandardButton.Cancel) != QMessageBox.StandardButton.Ok:
            return
        sel = self.worker_box.currentText()
        workers = None if sel == core.AUTOMATIC else int(sel)
        if not keep_rows:
            self.updating = False
            self.make_rows(False, self.prepare_now)
            self.set_step(1)
            self.build_title.setText("Building your game")
            self.update_bar.hide()
            self.ring_box.show()
        self.log_lines = []
        self.log_box.clear()
        self.current, self.finished = None, ""
        self.compiled, self.files_done, self.files_of = 0.0, 0, 0
        self.files_changed, self.files_compiled, self.compile_eta = -1, 0, -1.0
        self.this_compile, self.previous_total = 0, -1
        self.shaders_began, self.shaders_later, self.built_in = False, None, None
        self.show_page(self.build_page)
        self.cancel_button.show()
        self.set_progress(self.weight["read"][0] if self.updating else 0)
        self.elapsed0, self.compile0 = time.monotonic(), None
        self.clock.start()
        self.tick()
        if core.work_chosen() and "--work" not in args:
            args += ["--work", core.work()]
        if dlss and "--fetch-ngx-sdk" not in args:
            args += ["--fetch-ngx-sdk", "--accept-nvidia-license"]
        self.runner = core.Runner(tools, args, workers, core.ngx_sdk() if dlss else None,
                                  on_line=lambda l: self.post(self.line, l), on_finished=lambda f: self.post(self.ended, f))
        self.runner.start()

    FILES_LINE = re.compile(r"^(\d+) files, ")
    FILE_LINE = re.compile(r"^\s+\S+\s+\d+ MB\s+\d+s")

    def line(self, line):
        if line.startswith("@@stage "):
            sid = line.split(" ")[1]
            if self.current in self.rows:
                self.rows[self.current].done()
            self.current = sid
            if sid in self.rows:
                self.rows[sid].begin()
            if sid == "compile":
                self.compile0 = time.monotonic()
            if sid == "shaders":
                self.shaders_began = True
            if sid in self.weight:
                self.set_progress(self.weight[sid][0])
        elif line.startswith("@@progress "):
            try:
                f = float(line[11:])
            except ValueError:
                return
            if self.current in self.weight:
                if self.current == "compile":
                    self.compiled = f
                w = self.weight[self.current]
                self.set_progress(w[0] + w[1] * max(0.0, min(1.0, f)))
            return
        elif line.startswith(("@@plan ", "@@file ", "@@eta ", "@@times ")):
            self.number(line)
            return
        elif line.startswith("@@done "):
            self.finished = line[7:]
        elif NOT_PREPARED.match(line):
            self.shaders_later = shaders_note(NOT_PREPARED.match(line))
        elif self.FILES_LINE.match(line):
            self.files_of = int(self.FILES_LINE.match(line).group(1))
        elif self.FILE_LINE.match(line):
            self.files_done += 1
        if self.current == "compile" and self.files_changed < 0 and self.files_of > 0 and "compile" in self.rows:
            self.rows["compile"].small_text("%d of %d files" % (self.files_done, self.files_of))
        self.add_log(line)

    def number(self, line):
        p = line.split(" ")
        if p[0] == "@@plan" and len(p) == 3 and p[1].isdigit() and p[2].isdigit():
            self.files_changed, self.files_of, self.files_compiled = int(p[1]), int(p[2]), 0
            self.changed_files()
        elif p[0] == "@@file" and len(p) == 4 and p[3] in ("new", "again"):
            self.files_compiled += 1
            self.changed_files()
        elif p[0] == "@@eta" and len(p) == 2:
            try:
                self.compile_eta = float(p[1])
            except ValueError:
                pass
        elif p[0] == "@@times" and len(p) == 4:
            try:
                self.this_compile, self.previous_total = int(p[2]), int(p[3])
                self.built_in = time.monotonic() - (self.elapsed0 or time.monotonic())   # (without the shaders' minutes)
            except ValueError:
                pass

    def changed_files(self):
        if self.files_changed < 0 or "compile" not in self.rows:
            return
        self.rows["compile"].small_text("Everything is already built" if self.files_changed == 0 else
                                        "%d of %d files" % (self.files_compiled, self.files_changed) if self.files_changed >= self.files_of
                                        else "%d of %d changed files" % (self.files_compiled, self.files_changed))

    def add_log(self, line):
        self.log_lines.append(line)
        if len(self.log_lines) > 12000:
            del self.log_lines[:4000]
        if self.log_box.isVisible():
            self.log_box.appendPlainText(line)

    def set_progress(self, f):
        f = max(0.0, min(1.0, f))
        self.ring.set(f)
        self.update_bar.set(f)

    def tick(self):
        for r in self.rows.values():
            r.tick()
        if not self.runner:
            return
        tail = ". Your current game stays as it is until the new one is complete." if self.updating else " · your PC stays usable"
        if self.current == "compile" and self.compile0:
            el = time.monotonic() - self.compile0
            left = (time_left(self.compile_eta) if self.compile_eta >= 0 else
                    time_left(el / self.compiled * (1 - self.compiled) + 60) if self.compiled > 0.03 else "Estimating…")
            self.build_note.setText("Compiling %s · %s%s" % (clock(el), left, tail))
            return
        self.build_note.setText(("Working…" if self.updating else "Usually about 45 minutes") + tail)

    def ended(self, failure):
        self.clock.stop()
        self.runner = None
        if self.finished:
            for sid, r in self.rows.items():
                why = None if sid != "shaders" else (self.shaders_later if self.shaders_later else
                                                    None if self.shaders_began else "Left to the game's first start")
                if why:
                    r.skipped()
                    r.small_text(why)
                else:
                    r.done()
            self.set_progress(1)
            self.show_ready()
            return
        if failure == "cancelled":
            self.back()
            return
        if self.current in self.rows:
            self.rows[self.current].stopped()
        self.show_problem(failure or "The build tools stopped without saying why.", tools=bool(failure and "antivirus" in failure.lower()))

    def log_toggled(self, show):
        self.log_box.setVisible(show)
        self.step_card.setVisible(not show)
        self.show_log.setText(self.T("HideDetails" if show else "ShowDetails"))
        if show:
            self.log_box.setPlainText("\n".join(self.log_lines))
            self.log_box.verticalScrollBar().setValue(self.log_box.verticalScrollBar().maximum())

    def cancel_build(self):
        if self.download:
            self.download.cancel()
        if self.runner:
            self.runner.cancel()

    # ================= 3: play =================

    def show_ready(self):
        self.set_step(2)
        self.ready_title.setText("Your game is updated" if self.updating else "Your game is ready")
        took = time.monotonic() - (self.elapsed0 or time.monotonic())
        compiled = " (compiled in %s)" % span(self.this_compile) if self.this_compile > 0 else ""
        self.ready_note.setText("%s in %s%s%s · %s" % ("Updated" if self.updating else "Built", span(took), compiled, self.against(self.built_in or took),
                                                        self.finished))
        if not os.environ.get("HOOKSHOT_UI_PREVIEW"):
            if not self.updating:
                core.shortcuts(self.finished, self.desktop_tick.isChecked(), self.start_tick.isChecked())
            else:   # (the ones the player has, onto the folder's launcher; the ticks say which there are)
                self.desktop_tick.setChecked(os.path.isfile(core.desktop_link()))
                self.start_tick.setChecked(os.path.isfile(core.start_link()))
                core.refresh_shortcuts(self.finished)
        if core.shortcut_error:
            self.add_log("shortcut not made: " + core.shortcut_error)
        self.show_page(self.ready_page)
        import threading

        def run():
            if not core.keep_build_files():
                left = core.free(after_build=True)
                if left >= 100 << 20:
                    self.post(self.add_log, "build files: %s could not be deleted (a file in use); they go after the next build" %
                              core.gb(left))
            self.post(self.show_storage_use, *core.measure())
        threading.Thread(target=run, daemon=True).start()

    def show_storage_use(self, total, freeable, extra=""):
        self.storage_row.hide()
        if total < 100 << 20:
            return
        where = "%s and %s" % (paths.base(), core.work()) if core.work_chosen() else paths.base()
        self.storage_note.setText(
            "Hookshot Builder keeps %s on this PC (%s). %s of it only makes the next update quicker and can be freed: your game keeps "
            "playing, and the next update then takes about as long as this build." % (core.gb(total), where, core.gb(freeable))
            if freeable >= 100 << 20 else "Hookshot Builder keeps %s on this PC (%s): the builder itself." % (core.gb(total), where))
        self.storage_note.setText(self.storage_note.text() + extra)
        self.free_button.setVisible(freeable >= 100 << 20)
        self.free_button.setEnabled(True)
        self.free_button.setText("Free up space")
        self.storage_row.show()

    def free_space(self):
        self.free_button.setEnabled(False)
        self.free_button.setText("Freeing…")
        import threading

        def run():
            left = core.free()
            extra = (" (%s could not be deleted: a file is in use. Close the game and the builder, and delete the folder's contents by "
                     "hand.)" % core.gb(left)) if left >= 100 << 20 else ""
            self.post(self.show_storage_use, *core.measure(), extra)
        threading.Thread(target=run, daemon=True).start()

    def shortcuts_changed(self):
        if self.finished:
            core.shortcuts(self.finished, self.desktop_tick.isChecked(), self.start_tick.isChecked())

    def against(self, now):
        if self.previous_total <= 0:
            return ""
        diff = self.previous_total - now
        how = "about the same" if abs(diff) < 30 else "%s faster" % span(diff) if diff > 0 else "%s slower" % span(-diff)
        return " · last time %s (%s)" % (span(self.previous_total), how)

    def play(self):
        core.start_game(self.finished)
        self.showMinimized()

    # ================= when something goes wrong =================

    def show_problem(self, why, tools):
        self.last_failure, self.problem_is_tools = why, tools
        while self.problem_steps.count():
            it = self.problem_steps.takeAt(0)
            if it.widget():
                it.widget().deleteLater()
        self.problem_sub.setText("Your current game is unchanged and still plays." if self.updating else
                                 "Nothing was changed in your game folder.")
        if tools:
            self.problem_title.setText("The build tools need repairing")
            self.problem_head.setText("Your antivirus removed part of the build tools")
            self.problem_text.setText(why + "\n\nAntivirus programs sometimes flag unsigned developer tools by mistake.")
            for n, s in ((1, "Open Windows Security → Protection history, and restore the removed files."),
                         (2, "Or add %s to its exclusions." % paths.base()),
                         (3, "Then press Repair: Hookshot downloads the tools again from their official releases and checks each one.")):
                l = note("%d.  %s" % (n, s), 14)
                l.setContentsMargins(0, 6, 0, 0)
                self.problem_steps.addWidget(l)
        else:
            self.problem_title.setText("The update stopped" if self.updating else "The build stopped")
            self.problem_head.setText("What went wrong")
            self.problem_text.setText(why)
        self.repair_button.setText("Repair")
        self.repair_button.setVisible(tools)
        self.problem_details.setText("Show details")
        if not self.updating:
            self.set_step(1)
        self.show_page(self.problem_page)
        QTimer.singleShot(0, self.fit_problem)

    def fit_problem(self):
        w = self.problem_scroll.widget()
        w.adjustSize()
        h = w.layout().sizeHint().height() if w.layout() else w.sizeHint().height()
        h = w.layout().heightForWidth(self.problem_scroll.viewport().width()) if w.layout() and w.layout().hasHeightForWidth() else h
        self.problem_scroll.setFixedHeight(max(20, min(190, h + 2)))

    def problem_show_details(self):
        while self.problem_steps.count():
            it = self.problem_steps.takeAt(0)
            if it.widget():
                it.widget().deleteLater()
        box = LogBox(border=False)
        box.setPlainText("\n".join(self.log_lines[-80:]))
        box.setMinimumHeight(160)
        self.problem_steps.addWidget(box)
        self.problem_scroll.setFixedHeight(190)

    def copy_for_report(self):
        tools = core.Tools.find()
        import platform
        text = "Hookshot Builder %s on Windows %s\n%s: %s\n\n%s" % (tools.version if tools and tools.version else "(source)",
                                                                  platform.version(), self.problem_title.text(), self.last_failure,
                                                                  "\n".join(self.log_lines[-200:]))
        QGuiApplication.clipboard().setText(text)
        self.copy_report.setText("Copied")

    def back(self):
        """The first page again, from a build or an update that stopped or was cancelled. refresh_mode leaves the button alone
        while an update runs, and one that stopped left it so for good: the page kept the button it had before. After a start
        with --update that was a grey "Build the game" over a page with no game files and no saved choices, and the Update
        button only came back when the app was closed and opened again (#2129). The update is over here: the page is set up if
        it never was, for the update's folder, and its button says what that needs"""
        self.set_step(0)
        self.show_page(self.files_page)
        if not self.updating:
            return
        self.updating = False
        if self.files_ready:
            self.refresh()
        else:
            self.files_ready = True
            self.kept_folder = self.out_path.text()
            self.find_files()

    def repair(self):
        """The build tools checked against their checksums and downloaded again, after this window closes"""
        updater.restart(["--repair"])
        QApplication.quit()

    # ================= the update (updater.py) =================

    def begin_update(self, folder, tag):
        self.updating = True
        self.out_path.setText(folder)
        self.finished = ""
        to = tag or "the latest version"
        tools = core.Tools.find()
        frm = core.game_version(folder) or (tools.version if tools else None)
        self.step_pill.hide()
        self.version_chip.setText("%s → %s" % (frm, to) if frm and frm != to else to)
        self.version_chip.adjustSize()
        self.version_chip.show()
        self.prepare_now = not core.load_settings().get("PrepareLater")   # (the saved choice: an update never shows the tick)
        self.make_rows(True, self.prepare_now)
        self.build_title.setText("Updating your game")
        self.build_note.setText("Getting the update…")
        self.ring_box.hide()
        self.update_bar.show()
        self.set_progress(0)
        self.rows["download"].begin()
        self.cancel_button.show()
        self.show_page(self.build_page)

    def download_progress(self, f):
        if "download" in self.rows:
            self.rows["download"].small_text("%d%%" % int(f * 100))
        self.set_progress(self.weight["download"][1] * f)

    def note(self, line):
        self.add_log(line)
        if "download" in self.rows:
            self.rows["download"].small_text(line)

    def download_failed(self, why):
        self.download = None
        if "download" in self.rows:
            self.rows["download"].stopped()
        self.show_problem(why, tools=False)
        self.problem_title.setText("The update could not be downloaded")

    def update_build(self, tools, folder):
        """The new tools are here: the folder built again with them (only what changed is compiled)"""
        self.download = None
        if "download" in self.rows:
            self.rows["download"].done()
            self.rows["download"].small_text("Build tools checked")
        # (a folder built with DLSS keeps it: NVIDIA's license accepted, as the settings record it (DlssLicense, the WPF builder's
        # too), or for one accepted before the record, the SDK's LICENSE.txt. The SDK's files alone were the record, and with them
        # gone (deleted, a new builder folder) the update built without DLSS and its runtime went)
        dlss = os.path.isfile(os.path.join(folder, "ngx", "nvngx_dlss.dll")) and (
            bool(core.load_settings().get("DlssLicense")) or os.path.isfile(os.path.join(core.ngx_sdk(), "LICENSE.txt")))
        self.run(tools, ["--update", folder] + ([] if self.prepare_now else [SHADERS_LATER]), keep_rows=True, dlss=dlss)

    def quit_app(self):
        QApplication.quit()

    # ================= the window =================

    def close_window(self):
        if self.runner and QMessageBox.question(self, "Hookshot", self.T("CloseQuestion"),
                                                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No) != QMessageBox.StandardButton.Yes:
            return
        if self.runner:
            self.runner.cancel()
        self.close()


class ProblemCard(QFrame):
    """The problem's box: radius 10, #55E2776B border, #14E2776B fading to clear by 70%"""

    def paintEvent(self, e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        lg = QLinearGradient(r.topLeft(), r.bottomLeft())
        lg.setColorAt(0, argb("#14E2776B"))
        lg.setColorAt(0.7, argb("#00E2776B"))
        lg.setColorAt(1, argb("#00E2776B"))
        p.fillPath(rounded(r, 10), lg)
        p.setPen(QPen(argb("#55E2776B"), 1))
        p.drawPath(rounded(r, 10))


# ================= Glass.cs: Windows 11's acrylic, dark title colours and round corners (DWM) =================

def light_backdrop():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize") as k:
            for name in ("SystemUsesLightTheme", "AppsUseLightTheme"):
                try:
                    return winreg.QueryValueEx(k, name)[0] != 0
                except OSError:
                    continue
    except OSError:
        pass
    return False


class MARGINS(ctypes.Structure):
    _fields_ = [("l", ctypes.c_int), ("r", ctypes.c_int), ("t", ctypes.c_int), ("b", ctypes.c_int)]


def apply_glass(hwnd):
    """true: acrylic is on (the window's background stays see-through)"""
    import ctypes.wintypes
    dwm, user = ctypes.windll.dwmapi, ctypes.windll.user32
    GWL_STYLE = -16
    style = user.GetWindowLongPtrW(ctypes.c_void_p(hwnd), GWL_STYLE)
    # (a framed window: Windows draws the backdrop only behind one; without the system menu it draws no caption buttons of its own)
    style = (style | 0x00C00000 | 0x00040000) & ~0x00080000   # (WS_CAPTION | WS_THICKFRAME, no WS_SYSMENU)
    style &= ~0x00040000   # (no resizing border: the window is always 960x600)
    user.SetWindowLongPtrW(ctypes.c_void_p(hwnd), GWL_STYLE, ctypes.c_void_p(style))
    user.SetWindowPos(ctypes.c_void_p(hwnd), None, 0, 0, 0, 0, 0x0027)   # (SWP_FRAMECHANGED | NOMOVE | NOSIZE | NOZORDER)
    on = ctypes.c_int(1)
    dwm.DwmSetWindowAttribute(ctypes.c_void_p(hwnd), 20, ctypes.byref(on), 4)   # dark mode
    corner = ctypes.c_int(2)
    dwm.DwmSetWindowAttribute(ctypes.c_void_p(hwnd), 33, ctypes.byref(corner), 4)   # round corners
    if light_backdrop():
        return False
    if sys.getwindowsversion().build < 22621:   # (22H2: the backdrop attribute)
        return False
    hc = ctypes.wintypes.BOOL()
    m = MARGINS(-1, -1, -1, -1)
    dwm.DwmExtendFrameIntoClientArea(ctypes.c_void_p(hwnd), ctypes.byref(m))
    backdrop = ctypes.c_int(3)   # acrylic
    return dwm.DwmSetWindowAttribute(ctypes.c_void_p(hwnd), 38, ctypes.byref(backdrop), 4) == 0
