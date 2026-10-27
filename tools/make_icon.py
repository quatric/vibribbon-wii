# Generates the 128x48 Homebrew Channel icon (a CD next to the app name).
import sys
from PIL import Image, ImageDraw, ImageFont

img = Image.new("RGBA", (128, 48), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
# disc
for r, col in ((21, (200, 205, 215, 255)), (20, (225, 230, 240, 255))):
    d.ellipse((4 + 21 - r, 3 + 21 - r, 4 + 21 + r, 3 + 21 + r), fill=col)
for i, col in enumerate([(255, 120, 120, 120), (255, 220, 120, 120), (120, 220, 255, 120), (190, 140, 255, 120)]):
    d.pieslice((5, 4, 45, 44), 40 + i * 25, 60 + i * 25, fill=col)
d.ellipse((17, 16, 33, 32), fill=(160, 165, 175, 255))
d.ellipse((22, 21, 28, 27), fill=(0, 0, 0, 0))
try:
    font = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", 15)
except OSError:
    font = ImageFont.load_default()
d.text((50, 7), "Wii CD", font=font, fill=(255, 255, 255, 255))
d.text((50, 25), "Player", font=font, fill=(120, 200, 255, 255))
img.save(sys.argv[1])
