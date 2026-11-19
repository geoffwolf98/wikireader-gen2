#!/usr/bin/env python3
"""mkpiczim.py SOURCE.zim OUT.zim

A small "maxi" style ZIM for testing pictures: articles with mwoffliner
markup (infobox picture, figure with caption, maths formula, icon,
percent encoded names, lazy loading, <noscript> copy, PNG and JPEG, a
picture shared by two articles, a sub-page, a picture far down a long
page), with pictures taken from SOURCE (the climate change test ZIM).
"""
import io, sys
from libzim.reader import Archive
from libzim.writer import Creator, Item, StringProvider, Hint
from PIL import Image

src = Archive(sys.argv[1])

def pic(path):
    return bytes(src.get_entry_by_path(path).get_item().content)

COW = 'Cow_(Fleckvieh_breed)_Oeschinensee_Slaunger_2009-07-07.jpg.webp'
BAMBOO = 'Bamboo_forest.jpg.webp'
TURBO = 'Turbogenerator01.jpg.webp'
WOMAN = 'Woman_at_work%2C_Gujarat.jpg.webp'       # the entry name has %2C in it
MAP = 'Koppen-Geiger_Map_v2_World_1991–2020.svg.png.webp'
LAMB = 'Lamb_meat_(1).jpg.webp'

class Pic(Item):
    def __init__(s, path, data, mime): s.p, s.d, s.m = path, data, mime
    def get_path(s): return s.p
    def get_title(s): return ''
    def get_mimetype(s): return s.m
    def get_contentprovider(s): return StringProvider(s.d)
    def get_hints(s): return {Hint.FRONT_ARTICLE: False}

class Page(Item):
    def __init__(s, path, title, body): s.p, s.t, s.b = path, title, body
    def get_path(s): return s.p
    def get_title(s): return s.t
    def get_mimetype(s): return 'text/html'
    def get_contentprovider(s):
        return StringProvider('<!DOCTYPE html><html><head><meta charset="utf-8"><title>%s</title>'
                              '</head><body><div id="mw-content-text"><h1>%s</h1>%s</div></body></html>'
                              % (s.t, s.t, s.b))
    def get_hints(s): return {Hint.FRONT_ARTICLE: True}

def infobox(srcname, w, h, caption):
    return ('<table class="infobox"><tbody><tr><th colspan="2">%s</th></tr>'
            '<tr><td colspan="2" class="infobox-image"><span typeof="mw:File"><a href="./File:X" '
            'class="mw-file-description"><img alt="" src="%s" decoding="async" width="%d" height="%d" '
            'class="mw-file-element"></a></span><div class="infobox-caption">%s</div></td></tr>'
            '<tr><th>Kingdom</th><td>Animalia</td></tr></tbody></table>' % (caption, srcname, w, h, caption))

def figure(srcname, caption, attr='src'):
    return ('<figure typeof="mw:File/Thumb"><a href="./File:Y" class="mw-file-description">'
            '<img %s="%s" decoding="async" width="220" height="165" class="mw-file-element"></a>'
            '<figcaption>%s</figcaption></figure>' % (attr, srcname, caption))

def para(n, word='cattle'):
    return ''.join('<p>Paragraph %d about %s, with enough words to fill a line or two on the '
                   'small screen of the WikiReader.</p>' % (i + 1, word) for i in range(n))

MATH = ('<span class="mwe-math-element"><img src="./f8aab0483f099f025d5fe7174146ee315f166b31.svg" '
        'class="mwe-math-fallback-image-inline" alt="{\\displaystyle x^{2}}"></span>')
ICON = '<img src="./Flag_icon.webp" width="16" height="11" class="mw-file-element">'

# PNG and JPEG versions of a picture
lamb = Image.open(io.BytesIO(pic(LAMB))).convert('RGB')
b = io.BytesIO(); lamb.save(b, 'PNG'); LAMB_PNG = b.getvalue()
b = io.BytesIO(); lamb.save(b, 'JPEG', quality=85); LAMB_JPG = b.getvalue()
b = io.BytesIO(); Image.new('RGB', (16, 11), (200, 0, 0)).save(b, 'WEBP'); ICON_WEBP = b.getvalue()

pages = [
    ('Cattle', 'Cattle',
     infobox('./' + COW, 220, 147, 'A Fleckvieh cow') + para(3) + MATH + ICON +
     '<p>More about cattle and fields.</p>' + figure(BAMBOO, 'Bamboo, for no reason') + para(3)),
    ('Gujarat_work', 'Gujarat work',
     para(1, 'work') + figure('./Woman_at_work%252C_Gujarat.jpg.webp', 'A woman at work') + para(2, 'work')),
    ('Bamboo', 'Bamboo',
     '<noscript><img src="./' + TURBO + '" width="220" height="150"></noscript>' + para(1, 'bamboo') +
     figure('./' + BAMBOO, 'A lazy picture', 'data-src') + para(2, 'bamboo')),
    ('Lamb', 'Lamb',
     para(1, 'lamb') + figure('./Lamb_meat.png', 'As PNG') + para(1, 'lamb') +
     figure('./Lamb_meat.jpg', 'As JPEG') + para(1, 'lamb')),
    ('Shared', 'Shared',
     para(1, 'sharing') + figure('./' + COW, 'The same cow') + para(1, 'sharing')),
    ('Wind/Power', 'Wind power',
     para(1, 'wind') + figure('../' + TURBO, 'A turbine hall, from a sub-page') + para(1, 'wind')),
    ('Long_page', 'Long page',
     para(60, 'length') + figure(MAP, 'A map far down the page') + para(5, 'length')),
    ('Plain', 'Plain', para(4, 'nothing')),
    # big enough to stop while opening (tap to stop), and one needing
    # WIKIFONT.BIN (the font load is never stopped)
    ('Huge_page', 'Huge page', para(3000, 'bulk')),
    ('Kanji', 'Kanji', '<p>\u6f22\u5b57 \u65e5\u672c\u8a9e \u4e2d\u6587</p>' + para(3, 'kanji')),
]

with Creator(sys.argv[2]).config_indexing(False, 'eng').config_clustersize(64 * 1024) as c:
    c.set_mainpath('Cattle')
    for k, v in (('Title', 'Picture test'), ('Language', 'eng'), ('Creator', 'zimwiki tests'),
                 ('Publisher', 'zimwiki'), ('Date', '2026-11-14'), ('Description', 'pictures'),
                 ('Name', 'picture_test')):
        c.add_metadata(k, v)
    for p, t, body in pages:
        c.add_item(Page(p, t, body))
    c.add_redirection('Cow', 'Cow', 'Cattle', {Hint.FRONT_ARTICLE: True})
    for name in (COW, BAMBOO, TURBO, WOMAN, MAP, LAMB):
        c.add_item(Pic(name, pic(name), 'image/webp'))
    c.add_item(Pic('Lamb_meat.png', LAMB_PNG, 'image/png'))
    c.add_item(Pic('Lamb_meat.jpg', LAMB_JPG, 'image/jpeg'))
    c.add_item(Pic('Flag_icon.webp', ICON_WEBP, 'image/webp'))
    c.add_item(Pic('f8aab0483f099f025d5fe7174146ee315f166b31.svg', b'<svg xmlns="http://www.w3.org/2000/svg"/>',
                   'image/svg+xml'))
print('written', sys.argv[2])
