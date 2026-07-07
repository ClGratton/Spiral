from __future__ import annotations

from pathlib import Path
from xml.sax.saxutils import escape

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_LEFT
from reportlab.lib.pagesizes import A4, landscape
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import cm, mm
from reportlab.pdfbase.pdfmetrics import stringWidth
from reportlab.platypus import (
    BaseDocTemplate,
    Flowable,
    FrameBreak,
    KeepTogether,
    ListFlowable,
    ListItem,
    NextPageTemplate,
    PageBreak,
    PageTemplate,
    Paragraph,
    Spacer,
    Table,
    TableStyle,
)
from reportlab.platypus.frames import Frame
from reportlab.graphics.shapes import Drawing, Line, Polygon, Rect, String


ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "output" / "pdf"
PDF_PATH = OUT_DIR / "engine_architecture_reference.pdf"


PAGE_W, PAGE_H = A4
LAND_W, LAND_H = landscape(A4)

INK = colors.HexColor("#17202A")
MUTED = colors.HexColor("#5D6978")
BLUE = colors.HexColor("#2457A6")
CYAN = colors.HexColor("#2A9D8F")
GOLD = colors.HexColor("#C1872D")
RED = colors.HexColor("#B94A48")
GREEN = colors.HexColor("#3B8E4D")
PURPLE = colors.HexColor("#7353BA")
PAPER = colors.HexColor("#F7F8FA")
PALE_BLUE = colors.HexColor("#EAF1FF")
PALE_GREEN = colors.HexColor("#EAF7F0")
PALE_GOLD = colors.HexColor("#FFF5E4")
PALE_RED = colors.HexColor("#FDEEEE")
PALE_PURPLE = colors.HexColor("#F0ECFA")
GRID = colors.HexColor("#CBD3DD")


def p(text: str, style: ParagraphStyle) -> Paragraph:
    return Paragraph(text, style)


def link(label: str, url: str) -> str:
    return f'<a href="{escape(url)}" color="#2457A6">{escape(label)}</a>'


def bullet_list(items: list[str], style: ParagraphStyle, bullet_color=BLUE) -> ListFlowable:
    return ListFlowable(
        [ListItem(Paragraph(item, style), bulletColor=bullet_color) for item in items],
        bulletType="bullet",
        start="circle",
        leftIndent=13,
        bulletFontName="Helvetica",
        bulletFontSize=6,
        bulletOffsetY=2,
    )


def numbered_list(items: list[str], style: ParagraphStyle) -> ListFlowable:
    return ListFlowable(
        [ListItem(Paragraph(item, style)) for item in items],
        bulletType="1",
        leftIndent=16,
        bulletFontName="Helvetica-Bold",
        bulletFontSize=8,
    )


def mini_bullets(items: list[str], style: ParagraphStyle) -> list[Paragraph]:
    return [Paragraph("- " + escape(item), style) for item in items]


class SectionLabel(Flowable):
    def __init__(self, label: str, color=BLUE, width=460):
        super().__init__()
        self.label = label.upper()
        self.color = color
        self.width = width
        self.height = 18

    def wrap(self, avail_width, avail_height):
        self.width = min(self.width, avail_width)
        return self.width, self.height

    def draw(self):
        self.canv.setFillColor(self.color)
        self.canv.roundRect(0, 3, 66, 12, 2.5, stroke=0, fill=1)
        self.canv.setFillColor(colors.white)
        self.canv.setFont("Helvetica-Bold", 6.5)
        self.canv.drawCentredString(33, 6.7, self.label[:16])
        self.canv.setStrokeColor(self.color)
        self.canv.setLineWidth(0.7)
        self.canv.line(74, 9, self.width, 9)


class DecisionCard(Flowable):
    def __init__(self, title: str, body: str, color=BLUE, width=480):
        super().__init__()
        self.title = title
        self.body = body
        self.color = color
        self.width = width
        self.height = 54

    def wrap(self, avail_width, avail_height):
        self.width = avail_width
        return self.width, self.height

    def draw(self):
        c = self.canv
        c.setFillColor(colors.white)
        c.setStrokeColor(self.color)
        c.setLineWidth(1.0)
        c.roundRect(0, 0, self.width, self.height, 6, stroke=1, fill=1)
        c.setFillColor(self.color)
        c.roundRect(0, 0, 8, self.height, 4, stroke=0, fill=1)
        c.setFillColor(INK)
        c.setFont("Helvetica-Bold", 9.8)
        c.drawString(16, 34, self.title)
        c.setFillColor(MUTED)
        c.setFont("Helvetica", 8.0)
        lines = wrap_plain(self.body, self.width - 28, "Helvetica", 8.0)
        y = 22
        for line in lines[:3]:
            c.drawString(16, y, line)
            y -= 10


def wrap_plain(text: str, width: float, font: str, size: float) -> list[str]:
    words = text.split()
    lines: list[str] = []
    current: list[str] = []
    for word in words:
        trial = " ".join(current + [word])
        if stringWidth(trial, font, size) <= width or not current:
            current.append(word)
        else:
            lines.append(" ".join(current))
            current = [word]
    if current:
        lines.append(" ".join(current))
    return lines


class ArchitectureMap(Flowable):
    def __init__(self, width=LAND_W - 3.2 * cm, height=360):
        super().__init__()
        self.base_width = 780
        self.base_height = 360
        self.desired_width = width
        self.width = width
        self.height = height

    def wrap(self, avail_width, avail_height):
        self.width = min(avail_width, self.desired_width)
        self.height = self.base_height * (self.width / self.base_width)
        return self.width, self.height

    def draw(self):
        c = self.canv
        scale = self.width / self.base_width
        c.saveState()
        c.scale(scale, scale)
        w = self.base_width
        h = self.base_height
        c.setFillColor(colors.white)
        c.setStrokeColor(GRID)
        c.roundRect(0, 0, w, h, 10, stroke=1, fill=1)

        cols = [
            ("Creator Layer", 18, 245, PALE_GOLD, GOLD, ["Guided workflows", "AI agent", "Visual graphs", "C# scripts"]),
            ("Engine Core", 210, 245, PALE_BLUE, BLUE, ["C++ core", "Jobs/task graph", "Scene facade", "ECS chunks"]),
            ("Renderer", 402, 245, PALE_GREEN, CYAN, ["Visibility buffer", "Material resolve", "Clustered lighting", "Ray residuals"]),
            ("GPU/Data", 594, 245, PALE_PURPLE, PURPLE, ["NVRHI first", "Render graph", "Virtual textures", "Mesh pages"]),
        ]
        for title, x, y, fill, stroke, items in cols:
            self._box(c, x, y, 160, 86, title, items, fill, stroke)
        self._arrow(c, 178, 288, 205, 288, BLUE)
        self._arrow(c, 370, 288, 397, 288, BLUE)
        self._arrow(c, 562, 288, 589, 288, BLUE)

        self._box(c, 18, 124, 216, 78, "Asset Automation", ["Scan cleanup", "Auto LOD/meshlets", "Texture packing", "Lighting bake"], PALE_GOLD, GOLD)
        self._box(c, 266, 124, 216, 78, "Runtime Data Model", ["Immutable frame snapshots", "Command buffers", "Streaming feedback", "Profiling lanes"], PALE_BLUE, BLUE)
        self._box(c, 514, 124, 216, 78, "Quality Contract", ["No required TAA", "Coverage first", "Current-frame effects", "Debug confidence"], PALE_RED, RED)
        self._arrow(c, 234, 163, 262, 163, CYAN)
        self._arrow(c, 482, 163, 510, 163, CYAN)

        self._box(c, 68, 28, 190, 52, "User Promise", ["Easy first result", "Inspect/override everything"], colors.HexColor("#F7FAFF"), BLUE)
        self._box(c, 310, 28, 190, 52, "Technical Promise", ["Sharp motion", "Measured material realism"], colors.HexColor("#F7FAFF"), CYAN)
        self._box(c, 552, 28, 190, 52, "Production Promise", ["Profiler driven", "Small-team friendly"], colors.HexColor("#F7FAFF"), GOLD)
        c.restoreState()

    def _box(self, c, x, y, bw, bh, title, items, fill, stroke):
        c.setFillColor(fill)
        c.setStrokeColor(stroke)
        c.setLineWidth(1)
        c.roundRect(x, y, bw, bh, 6, stroke=1, fill=1)
        c.setFillColor(stroke)
        c.setFont("Helvetica-Bold", 9)
        c.drawString(x + 10, y + bh - 17, title)
        c.setFillColor(INK)
        c.setFont("Helvetica", 7.2)
        yy = y + bh - 31
        for item in items:
            c.circle(x + 12, yy + 2.5, 1.3, stroke=0, fill=1)
            c.drawString(x + 19, yy, item)
            yy -= 12

    def _arrow(self, c, x1, y1, x2, y2, color):
        c.setStrokeColor(color)
        c.setFillColor(color)
        c.setLineWidth(1.2)
        c.line(x1, y1, x2, y2)
        c.line(x2, y2, x2 - 5, y2 + 3)
        c.line(x2, y2, x2 - 5, y2 - 3)


class PipelineDiagram(Flowable):
    def __init__(self, width=LAND_W - 3.2 * cm, height=270):
        super().__init__()
        self.base_width = 780
        self.base_height = 270
        self.desired_width = width
        self.width = width
        self.height = height

    def wrap(self, avail_width, avail_height):
        self.width = min(avail_width, self.desired_width)
        self.height = self.base_height * (self.width / self.base_width)
        return self.width, self.height

    def draw(self):
        c = self.canv
        scale = self.width / self.base_width
        c.saveState()
        c.scale(scale, scale)
        w = self.base_width
        c.setFillColor(colors.white)
        c.setStrokeColor(GRID)
        c.roundRect(0, 0, w, self.base_height, 10, stroke=1, fill=1)

        stages = [
            ("Simulation", "gameplay, physics, animation", BLUE),
            ("Visibility", "GPU culling, HZB, occluders", CYAN),
            ("Material", "visibility ID -> compact G-buffer", GOLD),
            ("Base Light", "clustered lights, probes, BRDF", GREEN),
            ("Ray Residual", "classify, trace, reconstruct", PURPLE),
            ("Post", "coverage resolve, tone map", RED),
        ]
        x = 24
        y = 178
        bw = 112
        gap = 18
        for i, (title, body, color) in enumerate(stages):
            self._stage(c, x, y, bw, 54, title, body, color)
            if i < len(stages) - 1:
                self._arrow(c, x + bw + 1, y + 27, x + bw + gap - 3, y + 27, color)
            x += bw + gap

        lanes = [
            ("Opaque default", "visibility buffer -> material resolve -> compact G-buffer -> clustered/deferred lighting", 28, 102, PALE_BLUE, BLUE),
            ("Special materials", "Forward+ for glass, hair, eyes, particles, foliage coverage and simple paths", 28, 58, PALE_GREEN, CYAN),
            ("No temporal baseline", "effects must look acceptable in the current frame; history may schedule work, not blend visible color", 28, 14, PALE_RED, RED),
        ]
        for title, text, x0, y0, fill, stroke in lanes:
            c.setFillColor(fill)
            c.setStrokeColor(stroke)
            c.roundRect(x0, y0, w - 56, 32, 5, stroke=1, fill=1)
            c.setFillColor(stroke)
            c.setFont("Helvetica-Bold", 8)
            c.drawString(x0 + 9, y0 + 19, title)
            c.setFillColor(INK)
            c.setFont("Helvetica", 7.5)
            c.drawString(x0 + 112, y0 + 19, text)
        c.restoreState()

    def _stage(self, c, x, y, w, h, title, body, color):
        c.setFillColor(colors.white)
        c.setStrokeColor(color)
        c.roundRect(x, y, w, h, 6, stroke=1, fill=1)
        c.setFillColor(color)
        c.roundRect(x, y + h - 12, w, 12, 5, stroke=0, fill=1)
        c.setFillColor(colors.white)
        c.setFont("Helvetica-Bold", 7.2)
        c.drawCentredString(x + w / 2, y + h - 9, title.upper())
        c.setFillColor(INK)
        c.setFont("Helvetica", 7.4)
        lines = wrap_plain(body, w - 12, "Helvetica", 7.4)
        yy = y + 29
        for line in lines[:2]:
            c.drawCentredString(x + w / 2, yy, line)
            yy -= 10

    def _arrow(self, c, x1, y1, x2, y2, color):
        c.setStrokeColor(color)
        c.setLineWidth(1)
        c.line(x1, y1, x2, y2)
        c.line(x2, y2, x2 - 5, y2 + 3)
        c.line(x2, y2, x2 - 5, y2 - 3)


class StackDiagram(Flowable):
    def __init__(self, rows: list[tuple[str, str, object]], width=480, height=220):
        super().__init__()
        self.rows = rows
        self.width = width
        self.height = height

    def wrap(self, avail_width, avail_height):
        self.width = min(avail_width, self.width)
        return self.width, self.height

    def draw(self):
        c = self.canv
        w = self.width
        row_h = self.height / len(self.rows)
        y = self.height - row_h
        for title, text, color in self.rows:
            c.setFillColor(colors.white)
            c.setStrokeColor(color)
            c.roundRect(0, y + 3, w, row_h - 6, 5, stroke=1, fill=1)
            c.setFillColor(color)
            c.roundRect(0, y + 3, 92, row_h - 6, 5, stroke=0, fill=1)
            c.setFillColor(colors.white)
            c.setFont("Helvetica-Bold", 8)
            c.drawString(10, y + row_h / 2 - 3, title)
            c.setFillColor(INK)
            c.setFont("Helvetica", 7.7)
            for idx, line in enumerate(wrap_plain(text, w - 115, "Helvetica", 7.7)[:2]):
                c.drawString(105, y + row_h / 2 + 2 - 9 * idx, line)
            y -= row_h


def build_styles():
    ss = getSampleStyleSheet()
    styles = {}
    styles["Title"] = ParagraphStyle("Title", parent=ss["Title"], fontName="Helvetica-Bold", fontSize=25, leading=29, textColor=INK, spaceAfter=8)
    styles["Subtitle"] = ParagraphStyle("Subtitle", parent=ss["Normal"], fontName="Helvetica", fontSize=10.5, leading=15, textColor=MUTED, alignment=TA_CENTER)
    styles["H1"] = ParagraphStyle("H1", parent=ss["Heading1"], fontName="Helvetica-Bold", fontSize=17, leading=21, textColor=INK, spaceBefore=6, spaceAfter=8)
    styles["H2"] = ParagraphStyle("H2", parent=ss["Heading2"], fontName="Helvetica-Bold", fontSize=12.5, leading=15, textColor=BLUE, spaceBefore=8, spaceAfter=5)
    styles["Body"] = ParagraphStyle("Body", parent=ss["BodyText"], fontName="Helvetica", fontSize=8.7, leading=12.2, textColor=INK, spaceAfter=5)
    styles["Small"] = ParagraphStyle("Small", parent=ss["BodyText"], fontName="Helvetica", fontSize=7.3, leading=9.8, textColor=INK, spaceAfter=3)
    styles["Muted"] = ParagraphStyle("Muted", parent=ss["BodyText"], fontName="Helvetica", fontSize=7.5, leading=10, textColor=MUTED, spaceAfter=4)
    styles["Callout"] = ParagraphStyle("Callout", parent=ss["BodyText"], fontName="Helvetica-Bold", fontSize=8.7, leading=11.5, textColor=INK, backColor=colors.HexColor("#F2F6FC"), borderColor=GRID, borderWidth=0.7, borderPadding=6, spaceBefore=4, spaceAfter=6)
    styles["Code"] = ParagraphStyle("Code", parent=ss["Code"], fontName="Courier", fontSize=7.2, leading=9.2, textColor=colors.HexColor("#25303B"), backColor=colors.HexColor("#F3F5F7"), borderColor=GRID, borderWidth=0.5, borderPadding=5, spaceBefore=4, spaceAfter=6)
    styles["TOC"] = ParagraphStyle("TOC", parent=ss["BodyText"], fontName="Helvetica", fontSize=9, leading=13, textColor=INK, leftIndent=4, spaceAfter=2)
    styles["Caption"] = ParagraphStyle("Caption", parent=ss["BodyText"], fontName="Helvetica-Oblique", fontSize=7, leading=9, textColor=MUTED, alignment=TA_CENTER, spaceAfter=5)
    styles["TableHead"] = ParagraphStyle("TableHead", parent=ss["BodyText"], fontName="Helvetica-Bold", fontSize=7.2, leading=9, textColor=colors.white)
    styles["TableCell"] = ParagraphStyle("TableCell", parent=ss["BodyText"], fontName="Helvetica", fontSize=7.1, leading=9.2, textColor=INK)
    return styles


def table(data, widths, header=True):
    t = Table(data, colWidths=widths, hAlign="LEFT", repeatRows=1 if header else 0)
    ts = [
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("GRID", (0, 0), (-1, -1), 0.35, GRID),
        ("LEFTPADDING", (0, 0), (-1, -1), 5),
        ("RIGHTPADDING", (0, 0), (-1, -1), 5),
        ("TOPPADDING", (0, 0), (-1, -1), 4),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
    ]
    if header:
        ts += [
            ("BACKGROUND", (0, 0), (-1, 0), BLUE),
            ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
        ]
        if len(data) > 1:
            ts.append(("BACKGROUND", (0, 1), (-1, -1), colors.white))
            ts.append(("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, colors.HexColor("#FAFBFD")]))
    t.setStyle(TableStyle(ts))
    return t


def two_col(left, right, widths=(240, 240), gap=14):
    t = Table([[left, right]], colWidths=[widths[0], gap, widths[1]])
    t.setStyle(TableStyle([
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 0),
        ("RIGHTPADDING", (0, 0), (-1, -1), 0),
        ("TOPPADDING", (0, 0), (-1, -1), 0),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 0),
    ]))
    return t


def source_table(styles):
    refs = [
        ("Callisto Protocol character rendering", "https://advances.realtimerendering.com/s2023/SIGGRAPH2023-Advances-The-Rendering-of-The-Callisto-Protocol-JimenezPetersen.pdf"),
        ("Visibility Buffer, JCGT", "https://jcgt.org/published/0002/02/04/"),
        ("Visibility Buffer Rendering with Material Graphs", "https://filmicworlds.com/blog/visibility-buffer-rendering-with-material-graphs/"),
        ("Nanite SIGGRAPH 2021 course slides", "https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf"),
        ("NVIDIA RTX ray tracing best practices", "https://developer.nvidia.com/blog/rtx-best-practices/"),
        ("Detroit: Become Human lighting technology", "https://media.gdcvault.com/gdc2018/presentations/CAURANT_GUILLAUME_The_Lighting_Technology.pdf"),
        ("Light Field Probes", "https://research.nvidia.com/publication/2017-02_real-time-global-illumination-using-precomputed-light-field-probes"),
        ("XeGTAO", "https://github.com/GameTechDev/XeGTAO"),
        ("Frostbite stochastic SSR", "https://www.ea.com/news/stochastic-screen-space-reflections"),
        ("Humus triangulation note", "https://www.humus.name/index.php?ID=228&page=Comments"),
        ("Self Shadow, Counting Quads", "https://blog.selfshadow.com/2012/11/12/counting-quads/"),
        ("Threat Interactive Fox Engine video", "https://www.youtube.com/watch?v=aB5qxp6SPPQ"),
        ("MGSV graphics study", "https://www.adriancourreges.com/blog/2017/12/15/mgs-v-graphics-study/"),
        ("NVRHI", "https://github.com/NVIDIA-RTX/NVRHI"),
        ("NRI", "https://github.com/NVIDIA-RTX/NRI"),
        ("Slang shader language", "https://shader-slang.org/"),
        ("Microsoft .NET native hosting", "https://learn.microsoft.com/en-us/dotnet/core/tutorials/netcore-hosting"),
        ("Khronos PBR Neutral tone mapper", "https://github.com/KhronosGroup/ToneMapping/blob/main/PBR_Neutral/README.md"),
        ("ACES project", "https://www.oscars.org/science-technology/sci-tech-projects/aces"),
    ]
    rows = [[p("Reference", styles["TableHead"]), p("URL", styles["TableHead"])]]
    for title, url in refs:
        rows.append([p(escape(title), styles["TableCell"]), p(link(url, url), styles["TableCell"])])
    return table(rows, [150, 350])


def build_story(styles):
    S = styles
    story = []

    story += [
        Spacer(1, 28),
        p("Engine Architecture Reference", S["Title"]),
        p("A compact technical map of the engine direction, renderer choices, workflow philosophy, and first implementation priorities.", S["Subtitle"]),
        Spacer(1, 18),
        ArchitectureMap(width=PAGE_W - 3.2 * cm, height=300),
        Spacer(1, 12),
        p("Draft v0.1 - generated from the current architecture notes on 2026-07-06.", S["Caption"]),
        Spacer(1, 18),
        DecisionCard("Thesis", "Build a sharp-in-motion, measured-material, automation-first engine whose image quality does not depend on multi-frame temporal accumulation.", BLUE),
        Spacer(1, 8),
        DecisionCard("Core renderer", "Opaque rendering uses visibility buffer + material resolve + compact G-buffer + clustered/deferred lighting. Forward+ is reserved for coverage-heavy and special materials.", CYAN),
        Spacer(1, 8),
        DecisionCard("Product bet", "Small teams win when the engine automates LODs, materials, lighting, motion data, profiling, and guided workflows without hiding expert controls.", GOLD),
        PageBreak(),
    ]

    story += [
        SectionLabel("Contents", BLUE),
        p("How To Read This PDF", S["H1"]),
        p("This reference is organized from the engine promise down to the implementation contracts. Read the first five pages for the core idea, then jump into the subsystem pages when a decision needs detail.", S["Body"]),
        table(
            [
                [p("Section", S["TableHead"]), p("What it answers", S["TableHead"])],
                [p("North Star", S["TableCell"]), p("What the engine is trying to be and what it refuses to rely on.", S["TableCell"])],
                [p("Rendering Architecture", S["TableCell"]), p("Why the renderer is hybrid: visibility-buffer opaque path plus Forward+ carve-outs.", S["TableCell"])],
                [p("Materials And BRDF", S["TableCell"]), p("How the engine avoids the flat plastic look with Callisto/Proxima/GGX-style material controls.", S["TableCell"])],
                [p("Geometry And Assets", S["TableCell"]), p("How meshlets, cluster LOD, texture packing, scan conversion, and stable LOD transitions fit together.", S["TableCell"])],
                [p("Lighting And GI", S["TableCell"]), p("How probe/light-field lighting, screen-space correction, ray residuals, and daylight cycles integrate.", S["TableCell"])],
                [p("Runtime And Tools", S["TableCell"]), p("RHI, scripting language, DOTS-like concurrency, Hazel-inspired structure, automation, and profiling.", S["TableCell"])],
                [p("Roadmap And References", S["TableCell"]), p("What to build first, what remains undecided, and the papers/talks behind the choices.", S["TableCell"])],
            ],
            [160, 340],
        ),
        Spacer(1, 10),
        p("Legend: blue = core architecture, teal = rendering/GPU, gold = tools/automation, red = constraints and hazards, purple = optional/high-end paths.", S["Muted"]),
        PageBreak(),
    ]

    story += [
        SectionLabel("North Star", BLUE),
        p("1. Engine North Star", S["H1"]),
        p("The engine is a modern real-time game engine built around motion clarity, measured material realism, selective ray correction, and automation-first authoring. The renderer is not trying to become a low-sample path tracer, and it is not trying to copy old deferred engines or Unreal's accumulated legacy. Raster creates the stable high-frequency base. Rays correct the places raster is known to be wrong.", S["Body"]),
        p("Core equation:", S["H2"]),
        p("stable raster estimate + sparse ray-traced correction + same-frame spatial reconstruction", S["Code"]),
        p("Hard Rules", S["H2"]),
        bullet_list([
            "No required TAA, TSR, DLSS/FSR2-style temporal accumulation, or temporal denoisers as baseline image quality.",
            "Native-resolution rendering is the quality target; spatial upscalers can be optional performance modes.",
            "Every effect must look acceptable in the current frame by itself.",
            "Lambert is debug/fallback only, not the default diffuse model.",
            "Ray tracing is budgeted by perceptual and material importance, not spread evenly across the screen.",
        ], S["Small"], RED),
        p("What Success Looks Like", S["H2"]),
        bullet_list([
            "Texture and geometry remain readable in motion.",
            "Faces, eyes, skin, wet surfaces, metals, and rough materials have identifiable material behavior.",
            "Small teams get strong defaults and guided workflows.",
            "Debug views explain quality failures instead of hiding them with blur.",
            "Performance claims are backed by in-engine and vendor profiler captures.",
        ], S["Small"], GREEN),
        Spacer(1, 8),
        p("Design principle: static/offline data is allowed. Baked lightmaps, probe captures, BRDF fits, texture-space residuals, and asset preprocessing are not the same thing as screen-space temporal accumulation.", S["Callout"]),
        PageBreak(),
    ]

    story += [
        SectionLabel("System Map", BLUE, width=LAND_W - 3.2 * cm),
        p("2. Whole Engine System Map", S["H1"]),
        ArchitectureMap(),
        Spacer(1, 8),
        p("The engine is deliberately split into user-facing workflows, native runtime systems, renderer/RHI systems, and data/asset systems. The automation layer is not a toy wrapper: it produces real mesh clusters, LODs, material packs, motion databases, lighting caches, and profiler diagnostics.", S["Body"]),
        Spacer(1, 10),
        PipelineDiagram(),
        PageBreak(),
    ]

    story += [
        SectionLabel("Renderer", CYAN),
        p("3. Rendering Architecture", S["H1"]),
        p("The renderer is a hybrid. Opaque geometry follows a visibility-buffer path so the engine can determine what won visibility before doing expensive material work. Special materials use Forward+ because coverage, transparency, hair, particles, eyes, and glass do not fit a single-ID opaque visibility model cleanly.", S["Body"]),
        table(
            [
                [p("Path", S["TableHead"]), p("Used for", S["TableHead"]), p("Reason", S["TableHead"])],
                [p("Visibility buffer + compact G-buffer", S["TableCell"]), p("Dense opaque geometry, scans, hard-surface props, terrain, most characters.", S["TableCell"]), p("One material resolve per visible pixel, strong guide buffers for spatial reconstruction, less wasted material work under overdraw.", S["TableCell"])],
                [p("Clustered Forward+", S["TableCell"]), p("Glass, hair, particles, alpha-heavy VFX, foliage coverage, eyes, simple material profiles.", S["TableCell"]), p("Better for MSAA/coverage, translucency, and special shading paths.", S["TableCell"])],
                [p("Sparse ray residuals", S["TableCell"]), p("Shadow uncertainty, GI misses, reflection misses, hero details.", S["TableCell"]), p("Corrects raster/probe/SSR errors without making the whole image noisy.", S["TableCell"])],
            ],
            [130, 165, 205],
        ),
        Spacer(1, 8),
        StackDiagram([
            ("Visibility", "Depth + R32_UINT VisibilityID: drawClusterId:25 | localTriangleId:7. No global primitive table.", BLUE),
            ("Resolve", "Decode visible primitive, fetch material/instance/meshlet data, sample textures with explicit gradients.", CYAN),
            ("G-buffer", "Compact guide data: normal, roughness, material ID, coverage, reconstruction flags.", GOLD),
            ("Lighting", "Clustered/tiled lights, probe/light-field indirect, Callisto/Proxima/GGX BRDF.", GREEN),
            ("Residuals", "Classify important pixels, trace sparse rays, reconstruct spatially in the current frame.", PURPLE),
        ], width=500, height=230),
        Spacer(1, 6),
        p("Key contract: `VisibilityID` tells the renderer which draw cluster and triangle won. `MaterialID` or `brdfParamIndex` tells lighting which material/BRDF constants to fetch. These are separate identities.", S["Callout"]),
        PageBreak(),
    ]

    story += [
        SectionLabel("Frame Graph", CYAN),
        p("4. Low-Level Frame Flow", S["H1"]),
        p("The frame graph is arranged so visibility, material work, lighting, and rays each have a clear job. The selected occluder prepass is Fox-inspired, but modernized: it writes depth, coverage, and compatible visibility IDs only. It does not duplicate shaded G-buffer work.", S["Body"]),
        p(
            "Frame begin\\n"
            "  CPU task graph, gameplay/animation/physics jobs, streaming requests\\n"
            "Visibility\\n"
            "  selected occluder prepass -> HZB -> main cull -> main draw -> current HZB -> post cull\\n"
            "Material resolve\\n"
            "  count visible materials -> prefix sums -> pixel worklists -> compact G-buffer\\n"
            "Lighting base\\n"
            "  clustered light grid -> sun/sky -> probes/lightmaps -> GTAO -> shadow maps -> BRDF\\n"
            "Ray correction\\n"
            "  classify tiles -> SSR/SSGI candidates -> sparse rays -> spatial reconstruction\\n"
            "Special passes and post\\n"
            "  Forward+ translucency/hair/eyes/VFX -> coverage resolve -> CMAA2/SMAA -> tone map",
            S["Code"],
        ),
        table(
            [
                [p("Contract", S["TableHead"]), p("Decision", S["TableHead"])],
                [p("Occlusion", S["TableCell"]), p("Use two-pass HZB: previous HZB can only move work to a candidate list; current HZB decides the post pass.", S["TableCell"])],
                [p("Material bins", S["TableCell"]), p("Resolve worklist is sized for full screen/sample count. A single material may cover 100% of the frame.", S["TableCell"])],
                [p("Streaming", S["TableCell"]), p("Render thread never blocks on I/O. Missing data uses resident mip tails, coarse cluster ancestors, proxies, or explicit error resources.", S["TableCell"])],
                [p("Large worlds", S["TableCell"]), p("CPU can own large-coordinate world state; GPU rendering and ray queries use camera-relative translated space.", S["TableCell"])],
            ],
            [120, 380],
        ),
        PageBreak(),
    ]

    story += [
        SectionLabel("Materials", GOLD),
        p("5. Material And BRDF Model", S["H1"]),
        p("The default material model is Callisto-inspired across the whole scene, not only faces. The point is not to add post-process gloss; the point is to expose material controls that change the actual light response.", S["Body"]),
        table(
            [
                [p("Feature", S["TableHead"]), p("Why it exists", S["TableHead"]), p("Storage", S["TableHead"])],
                [p("Proxima diffuse", S["TableCell"]), p("Replaces Lambert for richer rough/organic diffuse response.", S["TableCell"]), p("Material flag + roughness/alpha input.", S["TableCell"])],
                [p("Diffuse Fresnel", S["TableCell"]), p("Controls grazing diffuse response.", S["TableCell"]), p("Constant or optional control texture.", S["TableCell"])],
                [p("Retroreflection", S["TableCell"]), p("Adds front-lit bounce-back control.", S["TableCell"]), p("Constant or optional control texture.", S["TableCell"])],
                [p("Smooth terminator", S["TableCell"]), p("Softens harsh curved-form light/shadow boundaries.", S["TableCell"]), p("Material parameter plus advanced length/tint.", S["TableCell"])],
                [p("GGX / dual GGX", S["TableCell"]), p("Main specular response plus optional second lobe for skin, eyes, wetness, cloth, paint.", S["TableCell"]), p("BRDF parameter buffer.", S["TableCell"])],
                [p("Specular Fresnel falloff", S["TableCell"]), p("Avoids one-size-fits-all specular behavior.", S["TableCell"]), p("Advanced tier parameter.", S["TableCell"])],
            ],
            [120, 245, 135],
        ),
        Spacer(1, 8),
        p("Material constants live in structured buffers. The G-buffer carries compact material/BRDF IDs and guide data; it does not store every Callisto parameter per pixel.", S["Callout"]),
        p("Baseline Texture Set", S["H2"]),
        bullet_list([
            "Base color: BC7/BC1, sRGB.",
            "Normal: BC5, reconstruct Z.",
            "ORM: AO/cavity, roughness, metallic.",
            "Height/depth: separate when parallax, displacement, or generation needs different mips.",
            "Callisto controls: optional, linear.",
        ], S["Small"], GOLD),
        p("Packing Rule", S["H2"]),
        bullet_list([
            "Pack only channels sharing color space, mip behavior, compression tolerance, sample frequency, and residency.",
            "Separate shadow/depth opacity if those passes do not need base color RGB.",
            "Roughness quality matters because bad roughness compression becomes specular shimmer without TAA.",
        ], S["Small"], GOLD),
        PageBreak(),
    ]

    story += [
        SectionLabel("Geometry", CYAN),
        p("6. Geometry, LOD, And Asset Processing", S["H1"]),
        p("The geometry policy is not classic hand-authored LOD only, and it is not Nanite at any density. Use a cluster hierarchy and GPU-driven selection, but target stable visible triangles and avoid fields of subpixel/quad-inefficient geometry.", S["Body"]),
        StackDiagram([
            ("Import", "Validate topology, split seams/materials, generate tangents, classify asset type.", BLUE),
            ("Optimize", "Vertex cache, overdraw, projected triangle shape/area, quad utilization, RT BVH quality.", CYAN),
            ("Bake", "Normals, height/depth, bent normal, cavity, curvature, micro-shadow hints.", GOLD),
            ("Cluster", "Build meshlets/clusters, hierarchy, page residency, coarse fallback.", GREEN),
            ("Runtime", "GPU culling, projected-error LOD, stable transitions, coverage-aware special paths.", PURPLE),
        ], width=500, height=220),
        Spacer(1, 8),
        table(
            [
                [p("Asset class", S["TableHead"]), p("Runtime representation", S["TableHead"])],
                [p("Static opaque scans", S["TableCell"]), p("Virtual cluster hierarchy with coarse resident fallback.", S["TableCell"])],
                [p("Foliage", S["TableCell"]), p("Coverage-aware Forward+/masked path, cards/clusters/aggregate voxels, alpha-to-coverage, stable masks.", S["TableCell"])],
                [p("Skinned characters", S["TableCell"]), p("Meshlets per LOD, GPU skinning, cluster hierarchy later where it proves stable.", S["TableCell"])],
                [p("Hair", S["TableCell"]), p("Hair meshes/strands/cards by distance with analytic coverage.", S["TableCell"])],
                [p("Wires/cables", S["TableCell"]), p("Analytic line/tube coverage where possible; avoid sliver triangle point sampling.", S["TableCell"])],
                [p("Captured backgrounds", S["TableCell"]), p("Optional Gaussian splats/radiance fields only for non-gameplay assets.", S["TableCell"])],
            ],
            [140, 360],
        ),
        Spacer(1, 8),
        p("LOD transitions use stable ordered/complementary dither or morphs. Frame-varying stochastic dither that needs TAA is not allowed as the baseline.", S["Callout"]),
        PageBreak(),
    ]

    story += [
        SectionLabel("Lighting", GREEN),
        p("7. Lighting, GI, Reflections, And Shadows", S["H1"]),
        p("Indirect lighting uses a stable probe/light-field backbone. Screen-space GI/AO and sparse RT are current-frame corrections. Static lightmaps can provide high-frequency static detail, but static and dynamic objects must consume one unified indirect lighting API.", S["Body"]),
        StackDiagram([
            ("Direct", "Current-frame sun, moon, local lights, shadow maps, and hero contact correction.", BLUE),
            ("Indirect", "Adaptive probe/light-field volume plus directional lightmaps where useful.", GREEN),
            ("Screen", "GTAO/XeGTAO, SSGI/contact bounce, SSR candidates with validity/confidence.", CYAN),
            ("Ray", "Sparse residuals for shadow uncertainty, GI misses, reflection misses, hero cases.", PURPLE),
            ("Fallback", "Sky SH/SG and calibrated debug ambient only when probe coverage fails.", GOLD),
        ], width=500, height=230),
        Spacer(1, 8),
        table(
            [
                [p("Problem", S["TableHead"]), p("Chosen solution", S["TableHead"])],
                [p("Dynamic object in baked room", S["TableCell"]), p("Same `IndirectLightingSample` path. Static can use lightmap detail; dynamic samples probes/light-fields plus contact AO/residuals.", S["TableCell"])],
                [p("Daylight cycle", S["TableCell"]), p("Dynamic direct sun/moon + adaptive time-keyed probe/lightmap indirect, not equal-time lightmap blending.", S["TableCell"])],
                [p("Close glossy reflection", S["TableCell"]), p("SSR candidate if valid, planar/RT for high-value or missing data, probes only for rough/distant tail.", S["TableCell"])],
                [p("Shadow cost", S["TableCell"]), p("Optimized shadow maps as base, receiver-aware caster culling/proxies, sparse RT visibility residuals for difficult areas.", S["TableCell"])],
                [p("AO", S["TableCell"]), p("XeGTAO/GTAO-style spatial AO with bent normals/specular occlusion; no AO path may require temporal accumulation.", S["TableCell"])],
            ],
            [130, 370],
        ),
        Spacer(1, 8),
        p("Detroit lesson adopted: adaptive probes, portal/zone GI blending, photometric units, material calibration, and debug tools. Detroit features rejected as baseline: Lambert diffuse and TAA-dependent shadows/volumetrics/SSR.", S["Callout"]),
        PageBreak(),
    ]

    story += [
        SectionLabel("AA + Color", RED),
        p("8. Antialiasing, Motion Clarity, And Color", S["H1"]),
        p("The antialiasing plan attacks instability at the source. Post AA is the cleanup layer, not the foundation. The color pipeline is treated as part of material realism: scene-referred HDR lighting, calibrated exposure, documented tone mapper, then artistic grading.", S["Body"]),
        table(
            [
                [p("Layer", S["TableHead"]), p("Purpose", S["TableHead"])],
                [p("Geometry/LOD prefiltering", S["TableCell"]), p("Avoid subpixel triangle fields and quad-waste before rasterization.", S["TableCell"])],
                [p("Coverage/MSAA/analytic edges", S["TableCell"]), p("Prevent binary crawling on silhouettes, foliage, hair, wires, and cluster boundaries.", S["TableCell"])],
                [p("Alpha-to-coverage", S["TableCell"]), p("Coverage-aware masked materials, not single-ID opaque visibility when overlap matters.", S["TableCell"])],
                [p("Specular/normal filtering", S["TableCell"]), p("Roughness remapping, mip correctness, normal-map filtering, specular AA.", S["TableCell"])],
                [p("CMAA2/SMAA cleanup", S["TableCell"]), p("Final spatial edge cleanup while preserving sharpness better than FXAA.", S["TableCell"])],
            ],
            [150, 350],
        ),
        Spacer(1, 10),
        p("Tone Mapper Candidates", S["H2"]),
        bullet_list([
            "Gran Turismo-style / neutral shoulder for crisp game output.",
            "AgX-style profile for natural highlight handling.",
            "ACES/filmic for cinematic consistency.",
            "Khronos PBR Neutral for asset/material validation.",
        ], S["Small"], RED),
        p("Validation Scenes", S["H2"]),
        bullet_list([
            "Neutral gray, saturated colors, metals, skin, wet surfaces.",
            "Daylight, indoor mixed lighting, night, emissives.",
            "Camera pans at 30, 60, 90, and 120 Hz.",
            "Path-traced/offline references for material response.",
        ], S["Small"], RED),
        PageBreak(),
    ]

    story += [
        SectionLabel("Runtime", BLUE),
        p("9. RHI, Language, And Runtime Model", S["H1"]),
        p("The engine core is C++. Gameplay scripting is C#/.NET. Visual graphs are a front-end that compile to C#, native IR, Slang, or job graph nodes depending on domain. Hot entity-scale runtime data uses a DOTS-like archetype/chunk model behind an approachable scene facade.", S["Body"]),
        table(
            [
                [p("Area", S["TableHead"]), p("Choice", S["TableHead"]), p("Reason", S["TableHead"])],
                [p("Engine core", S["TableCell"]), p("Modern C++23/26-style C++.", S["TableCell"]), p("Native renderer/RHI/streaming/physics ecosystem, deterministic lifetime, SIMD-friendly data.", S["TableCell"])],
                [p("RHI", S["TableCell"]), p("Engine::RHI facade, NVRHI first, NRI later/advanced, bgfx not main renderer.", S["TableCell"]), p("NVRHI gives guardrails while the architecture matures; NRI remains lower-level candidate.", S["TableCell"])],
                [p("Gameplay scripting", S["TableCell"]), p("C# on .NET 10 LTS through hostfxr/CoreCLR.", S["TableCell"]), p("Familiar to Unity developers and approachable for small teams; supports hot reload and tooling.", S["TableCell"])],
                [p("Visual scripting", S["TableCell"]), p("Blueprint-like graph editor over generated code/IR.", S["TableCell"]), p("Beginner friendly without a slow opaque VM in hot paths.", S["TableCell"])],
                [p("Shaders", S["TableCell"]), p("Slang-first HLSL-compatible authoring.", S["TableCell"]), p("Modern modular shaders with D3D12/Vulkan targets and future-proof backend options.", S["TableCell"])],
                [p("Concurrency", S["TableCell"]), p("Native job system + frame task graph + data snapshots.", S["TableCell"]), p("Renderer, animation, physics, streaming, baking, and tools can scale across CPU cores.", S["TableCell"])],
            ],
            [100, 170, 230],
        ),
        Spacer(1, 8),
        p("Hazel decision: use Hazel/The Cherno as a readability and skeleton reference, not as the engine base. Its renderer and product assumptions do not match this architecture.", S["Callout"]),
        PageBreak(),
    ]

    story += [
        SectionLabel("Automation", GOLD),
        p("10. Product Workflow And Automation", S["H1"]),
        p("The engine is meant to win small studios, solo developers, students, modders, and ambitious amateurs away from Unreal/Unity by combining advanced renderer defaults with guided creation. Automation must produce real engine data, not vague presets.", S["Body"]),
        p("Default onboarding funnel:", S["H2"]),
        p("choose game type -> create playable base -> choose visual style -> import/author content -> tune feel -> profile -> package", S["Code"]),
        table(
            [
                [p("Workflow", S["TableHead"]), p("Automated outputs", S["TableHead"])],
                [p("Asset import", S["TableCell"]), p("Topology cleanup, meshlets, LODs, impostors/cards, collision, texture compression, streaming pages, diagnostics.", S["TableCell"])],
                [p("Animation import", S["TableCell"]), p("Retargeting, motion-matching data, trajectory features, foot locking, inertialization settings, controller defaults.", S["TableCell"])],
                [p("Game type setup", S["TableCell"]), p("Camera, controller, interaction model, save/load, input, starter UI, test map, performance target.", S["TableCell"])],
                [p("Visual style", S["TableCell"]), p("Realism/stylization controls mapped to material richness, shadows, texture sharpness, foliage density, ray budget, tone mapper.", S["TableCell"])],
                [p("Optimization", S["TableCell"]), p("Frame time, pass cost, overdraw, quad waste, texture residency, material complexity, ray density, reflection/AO confidence.", S["TableCell"])],
            ],
            [120, 380],
        ),
        Spacer(1, 8),
        p("AI assistance is a workflow layer, not a replacement for deterministic tools. The same guided workflows must work without AI, and every automatic decision must be inspectable, overridable, and explainable.", S["Callout"]),
        PageBreak(),
    ]

    story += [
        SectionLabel("Roadmap", BLUE),
        p("11. First Implementation Slice", S["H1"]),
        p("Build the engine in a sequence that proves the core image-quality ideas before expanding into every feature. The first renderer slice should validate visibility, material resolve, BRDF, shadow residuals, and non-temporal clarity.", S["Body"]),
        numbered_list([
            "Engine core modules: Core, Jobs, Application, Platform, RHI, RenderGraph.",
            "NVRHI backend behind Engine::RHI plus render-graph/resource validation.",
            "Bindless resource tables and GPU scene buffers.",
            "Meshlet import path with quantized vertices, optimized indices, and cluster metadata.",
            "Visibility buffer with packed R32_UINT draw-cluster IDs.",
            "Material resolve into compact G-buffer and material-ID/BRDF table lookup.",
            "Clustered light grid and Callisto + Proxima + GGX BRDF shader.",
            "Shadow-map base lighting plus sparse RT shadow residual classifier/reconstruction.",
            "Coverage/MSAA + CMAA2/SMAA image clarity test without TAA.",
            "Basic adaptive probe volume and unified IndirectLightingSample path.",
            "C#/.NET host, hot reload, generated bindings, restricted job API, visual graph prototype.",
            "Guided import/profiling workflows and debug overlays.",
        ], S["Body"]),
        Spacer(1, 8),
        table(
            [
                [p("Still open", S["TableHead"]), p("Why it remains open", S["TableHead"])],
                [p("Probe v0 structure", S["TableCell"]), p("Sparse brick grid, octree, cascaded grid, artist probes, or hybrid should be chosen after prototype scenes.", S["TableCell"])],
                [p("Default tone mapper", S["TableCell"]), p("GT-style, AgX, ACES/filmic, and Khronos PBR Neutral need material-scene validation.", S["TableCell"])],
                [p("Per-sample visibility", S["TableCell"]), p("Opaque MSAA/coverage-heavy edges need exact storage tradeoffs measured.", S["TableCell"])],
                [p("Backend 2", S["TableCell"]), p("NRI or custom Vulkan/D3D12 should wait until the render graph is mature.", S["TableCell"])],
                [p("ECS implementation", S["TableCell"]), p("Study Flecs, EnTT, Bevy ECS, Unity DOTS, and Unreal MassEntity, then build the engine-owned runtime contract.", S["TableCell"])],
            ],
            [130, 370],
        ),
        PageBreak(),
    ]

    story += [
        SectionLabel("Reference", BLUE),
        p("12. Quick Decision Matrix", S["H1"]),
        table(
            [
                [p("Topic", S["TableHead"]), p("Decision", S["TableHead"])],
                [p("Temporal accumulation", S["TableCell"]), p("Rejected as required baseline. History may schedule work, not blend visible color.", S["TableCell"])],
                [p("Renderer", S["TableCell"]), p("Visibility-buffer opaque path + compact G-buffer + clustered/deferred lighting; Forward+ carve-outs.", S["TableCell"])],
                [p("BRDF", S["TableCell"]), p("Callisto/Proxima/GGX-style material family globally; Lambert debug/fallback only.", S["TableCell"])],
                [p("GI", S["TableCell"]), p("Adaptive probe/light-field backbone; lightmaps as static detail; screen-space and RT as correction.", S["TableCell"])],
                [p("Reflections", S["TableCell"]), p("SSR candidate, planar/RT for important missing data, probes for rough/distant fallback.", S["TableCell"])],
                [p("Shadows", S["TableCell"]), p("Optimized shadow maps + receiver-aware culling/proxies + sparse RT residual correction.", S["TableCell"])],
                [p("Geometry", S["TableCell"]), p("Cluster hierarchy with projected-error LOD; avoid subpixel/quad-inefficient density.", S["TableCell"])],
                [p("RHI", S["TableCell"]), p("Engine::RHI, NVRHI first, NRI/custom later, bgfx only for tools if useful.", S["TableCell"])],
                [p("Language", S["TableCell"]), p("C++ core, C# gameplay, visual graphs as generated code/IR, Slang shaders.", S["TableCell"])],
                [p("Concurrency", S["TableCell"]), p("Native job system, frame task graph, immutable render snapshots, async asset/build pipelines.", S["TableCell"])],
            ],
            [130, 370],
        ),
        PageBreak(),
    ]

    story += [
        SectionLabel("Sources", BLUE),
        p("References", S["H1"]),
        p("The PDF condenses the current architecture notes and the following talks, papers, source repositories, and technical documentation.", S["Body"]),
        source_table(S),
    ]
    return story


def draw_header_footer(canvas, doc):
    canvas.saveState()
    page_num = canvas.getPageNumber()
    width, height = doc.pagesize
    canvas.setFillColor(MUTED)
    canvas.setFont("Helvetica", 7)
    canvas.drawString(doc.leftMargin, 10 * mm, "Engine Architecture Reference")
    canvas.drawRightString(width - doc.rightMargin, 10 * mm, f"Page {page_num}")
    canvas.setStrokeColor(colors.HexColor("#D8DEE8"))
    canvas.setLineWidth(0.4)
    canvas.line(doc.leftMargin, 15 * mm, width - doc.rightMargin, 15 * mm)
    canvas.restoreState()


def make_pdf():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    styles = build_styles()
    doc = BaseDocTemplate(
        str(PDF_PATH),
        pagesize=A4,
        rightMargin=1.6 * cm,
        leftMargin=1.6 * cm,
        topMargin=1.55 * cm,
        bottomMargin=1.7 * cm,
        title="Engine Architecture Reference",
        author="Codex",
        subject="Game engine architecture and rendering decisions",
    )
    portrait_frame = Frame(doc.leftMargin, doc.bottomMargin, doc.width, doc.height, id="portrait")
    landscape_doc_width = LAND_W - 3.2 * cm
    landscape_frame = Frame(1.6 * cm, 1.55 * cm, landscape_doc_width, LAND_H - 3.25 * cm, id="landscape")
    doc.addPageTemplates([
        PageTemplate(id="Portrait", pagesize=A4, frames=[portrait_frame], onPage=draw_header_footer),
        PageTemplate(id="Landscape", pagesize=landscape(A4), frames=[landscape_frame], onPage=draw_header_footer),
    ])
    story = build_story(styles)
    doc.build(story)
    return PDF_PATH


if __name__ == "__main__":
    print(make_pdf())
