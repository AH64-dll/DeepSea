"""Deep Sea emblem: 16x16 pixel art -> deepsea.ico (16/32/48/256), deepsea.png, deepsea.svg."""
from pathlib import Path
from PIL import Image

GRID = [
    "..SSSSSSSSSSSS..",
    ".SSSSSSkSSSSyyS.",
    "SSSSSSSkcSSyyyyS",
    "SSSSSSSkccSyyyyS",
    "SSSSSSSkcccSyySS",
    "SSSSSSSkrrrrSSSS",
    "SSSSSSSkcccccSSS",
    "SSSSSSSkccccccSS",
    "SSShhhhhhhhhhhSS",
    "SSSwhhhhhhhhhwSS",
    "1111wwwwwwwww111",
    "1ww1111111111ww1",
    "2222222ww2222222",
    "3333o33333333333",
    ".44444444o44444.",
    "..444444444444..",
]
PALETTE = {
    "S": "#5BC0F8", "y": "#FFD65A", "k": "#1F2740", "c": "#FFF6DC", "r": "#E4573D",
    "h": "#A45A2F", "w": "#FFFFFF", "1": "#2E7FE0", "2": "#1B5CC2", "3": "#0E3E96",
    "4": "#072B6E", "o": "#8FC4F5",
}
assert all(len(row) == 16 for row in GRID) and len(GRID) == 16

here = Path(__file__).resolve().parent
base = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
for y, row in enumerate(GRID):
    for x, ch in enumerate(row):
        if ch != ".":
            h = PALETTE[ch].lstrip("#")
            base.putpixel((x, y), tuple(int(h[i:i + 2], 16) for i in (0, 2, 4)) + (255,))
big = base.resize((256, 256), Image.NEAREST)
big.save(here / "deepsea.png")
big.save(here / "deepsea.ico", sizes=[(16, 16), (32, 32), (48, 48), (256, 256)])

# SVG: one <path> per colour, unit squares merged into horizontal runs.
parts = []
for ch, colour in PALETTE.items():
    d = []
    for y, row in enumerate(GRID):
        x = 0
        while x < 16:
            if row[x] == ch:
                start = x
                while x < 16 and row[x] == ch:
                    x += 1
                d.append(f"M{start} {y}h{x - start}v1h-{x - start}z")
            else:
                x += 1
    if d:
        parts.append(f'<path fill="{colour}" d="{"".join(d)}"/>')
svg = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" shape-rendering="crispEdges">'
       + "".join(parts) + "</svg>")
(here / "deepsea.svg").write_text(svg + "\n", encoding="utf-8")
print(len(svg), "bytes of SVG")
