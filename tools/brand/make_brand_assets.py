"""mk9-lauta's brand pictures for the game's screens (app/src/brand) and for Discord's Rich Presence (docs/assets).

  python tools/brand/make_brand_assets.py <logo.png>

<logo.png>: the "MK9 BY LAUTA" logo on a transparent background (any size). Writes:
  app/src/brand/logo_1024.png      the logo cropped to what it draws, 1024 px wide (the first start's welcome)
  app/src/brand/logo_640.png, logo_400.png, logo_240.png   the same, narrower (each drawn from the nearest: no mipmaps)
  app/src/brand/social_icons.png   the social networks' icons, white on transparent: three rows of six cells (128, 64
                                   and 32 px), each drawn from the one nearest its size (no mipmaps in the overlay)
  docs/assets/discord_logo.png     512x512: the logo on a dark square, Rich Presence's picture (by its URL)

The icons are Simple Icons' (CC0 1.0, simpleicons.org; the same paths as the project's site), drawn by Microsoft
Edge in headless mode at 256 px and scaled down with Lanczos.
"""
import json
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
BRAND = os.path.join(ROOT, 'app', 'src', 'brand')
ASSETS = os.path.join(ROOT, 'docs', 'assets')

# the order is brand.cpp's kSocial (Discord, WhatsApp, Instagram, Kick, YouTube, X): social_icons.json, the
# paths as the project's site has them
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'social_icons.json'), encoding='utf-8') as _f:
    ICONS = [(i['name'], i['path']) for i in json.load(_f)['icons']]
CELL = 256  # Edge's drawing; the atlas's rows are 128, 64 and 32
ROWS = (128, 64, 32)


def edge():
    for path in (r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
                 r'C:\Program Files\Microsoft\Edge\Application\msedge.exe'):
        if os.path.exists(path):
            return path
    sys.exit('Microsoft Edge not found (it draws the icons)')


def draw_icons(work):
    """The six icons side by side, white on transparent, CELL px each (a margin of 1/24 around each)."""
    svgs = ''.join(f'<svg width="{CELL}" height="{CELL}" viewBox="-1 -1 26 26"><path fill="#fff" d="{d}"/></svg>'
                   for _, d in ICONS)
    page = os.path.join(work, 'icons.html')
    with open(page, 'w', encoding='utf-8') as f:
        f.write('<!doctype html><html><head><style>html,body{margin:0;padding:0;background:transparent;}'
                f'body{{display:flex;width:{CELL * len(ICONS)}px;height:{CELL}px;overflow:hidden;}}'
                f'svg{{display:block;flex:none;}}</style></head><body>{svgs}</body></html>')
    shot = os.path.join(work, 'icons.png')
    subprocess.run([edge(), '--headless=new', '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1',
                    '--default-background-color=00000000', f'--window-size={CELL * len(ICONS)},{CELL}',
                    f'--screenshot={shot}', 'file:///' + page.replace('\\', '/')],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60)
    image = Image.open(shot).convert('RGBA')
    if image.size != (CELL * len(ICONS), CELL):
        image = image.crop((0, 0, CELL * len(ICONS), CELL))
    return image


def icon_atlas(strip):
    """Rows of 128, 64 and 32 px cells; white everywhere with the coverage as alpha (no dark fringe when scaled)."""
    alpha = strip.getchannel('A')
    atlas = Image.new('L', (ROWS[0] * len(ICONS), sum(ROWS)), 0)
    y = 0
    for size in ROWS:
        for i in range(len(ICONS)):
            cell = alpha.crop((i * CELL, 0, (i + 1) * CELL, CELL)).resize((size, size), Image.LANCZOS)
            atlas.paste(cell, (i * size, y))
        y += size
    white = Image.new('RGBA', atlas.size, (255, 255, 255, 0))
    white.putalpha(atlas)
    return white


def cropped_logo(path):
    logo = Image.open(path).convert('RGBA')
    box = logo.getchannel('A').point(lambda a: 255 if a > 8 else 0).getbbox()
    margin = 8
    box = (max(0, box[0] - margin), max(0, box[1] - margin), min(logo.width, box[2] + margin),
           min(logo.height, box[3] + margin))
    return logo.crop(box)


def scaled(image, width):
    return image.resize((width, round(image.height * width / image.width)), Image.LANCZOS)


def discord_square(logo):
    """512x512: a dark red glow behind the logo, black at the corners (Discord rounds it)."""
    size = 512
    glow = Image.new('L', (size, size), 0)
    ImageDraw.Draw(glow).ellipse((60, 110, size - 60, size - 110), fill=255)
    glow = glow.filter(ImageFilter.GaussianBlur(70))
    base = Image.new('RGBA', (size, size), (8, 5, 5, 255))
    red = Image.new('RGBA', (size, size), (92, 18, 10, 255))
    base = Image.composite(red, base, glow)
    mark = scaled(logo, 470)
    base.alpha_composite(mark, ((size - mark.width) // 2, (size - mark.height) // 2))
    return base.convert('RGB')


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    os.makedirs(BRAND, exist_ok=True)
    os.makedirs(ASSETS, exist_ok=True)
    logo = cropped_logo(sys.argv[1])
    for width in (1024, 640, 400, 240):
        scaled(logo, width).save(os.path.join(BRAND, f'logo_{width}.png'), optimize=True)
    discord_square(logo).save(os.path.join(ASSETS, 'discord_logo.png'), optimize=True)
    with tempfile.TemporaryDirectory() as work:
        icon_atlas(draw_icons(work)).save(os.path.join(BRAND, 'social_icons.png'), optimize=True)
    for name in ('logo_1024.png', 'logo_640.png', 'logo_400.png', 'logo_240.png', 'social_icons.png'):
        p = os.path.join(BRAND, name)
        print(f'{p}: {Image.open(p).size}, {os.path.getsize(p) // 1024} KB')
    p = os.path.join(ASSETS, 'discord_logo.png')
    print(f'{p}: {os.path.getsize(p) // 1024} KB')


if __name__ == '__main__':
    main()
