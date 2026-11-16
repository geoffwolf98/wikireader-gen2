#!/usr/bin/env python3
# A ZIM of awkward HTML for tests/slimcheck: slimming must not change how
# any of these convert.  Usage: mkslimzim.py OUT.zim
import sys
from libzim.writer import Creator, Item, StringProvider, Hint

class Page(Item):
    def __init__(s, path, title, html): s.p, s.t, s.h = path, title, html
    def get_path(s): return s.p
    def get_title(s): return s.t
    def get_mimetype(s): return 'text/html'
    def get_contentprovider(s): return StringProvider(s.h)
    def get_hints(s): return {Hint.FRONT_ARTICLE: True}

def page(title, body, head=''):
    return ('<!DOCTYPE html>\n<html class="client-js"><head><meta charset="utf-8">'
            '<title>%s</title><link rel="stylesheet" href="../-/style.css">'
            '<script src="../-/a.js"></script><style>p{color:red}</style>%s</head>\n'
            '<body class="mediawiki"><!-- a comment --><div id="mw-content-text" class="mw-body">'
            '<h1 id="firstHeading" class="firstHeading">%s</h1>\n%s\n'
            '<!--htdig_noindex--><div class="licence">Licence text</div><!--/htdig_noindex-->'
            '</div></body></html>' % (title, head, title, body))

PAGES = {
 'Ticks': '''<table class="wikitable"><tr><th>Feature</th><th>Has</th></tr>
<tr><td>Wings</td><td><span typeof="mw:File"><img src="../I/Yes_check.svg.png" width="13" height="13"></span><span style="display:none">Y</span></td></tr>
<tr><td>Fins</td><td><span typeof="mw:File"><img src="../I/X_mark.svg.png" width="13"></span><span style="display: none">N</span></td></tr>
<tr><td>Hidden</td><td><span style="display:none">not shown</span> shown</td></tr></table>''',
 'Code': '''<p>Before</p><pre class="mw-code">  indented
	tab	here
line &amp; three</pre><p>After   with    spaces
and newlines</p><PRE>UPPER   pre</PRE>''',
 'Maths': '''<p>Inline <span class="mwe-math-element"><span class="mwe-math-mathml-inline mwe-math-mathml-a11y" style="display: none;"><math xmlns="http://www.w3.org/1998/Math/MathML" alttext="{\\displaystyle a&gt;b}"><semantics><mrow><mi>a</mi><mo>&gt;</mo><mi>b</mi></mrow></semantics></math></span><img src="../I/math.svg" class="mwe-math-fallback-image-inline mw-invert skin-invert" aria-hidden="true" alt="{\\displaystyle a&gt;b}"></span> text.</p>
<dl><dd><span class="mwe-math-element"><span class="mwe-math-mathml-display mwe-math-mathml-a11y" style="display: none;"><math display="block" alttext="{\\begin{pmatrix}1&amp;2\\\\3&amp;4\\end{pmatrix}}"><mtable><mtr><mtd>1</mtd></mtr></mtable></math></span><img src="../I/m2.svg" class="mwe-math-fallback-image-display" alt="matrix"></span></dd></dl>
<p>Block <math display="block" alttext="x^2 + y_1 = \\frac{1}{2}"><mi>x</mi></math> after.</p>''',
 'Notes': '''<p>A claim.<sup id="cite_ref-1" class="reference"><a href="#cite_note-1">[1]</a></sup> A note.<sup id="cite_ref-a" class="reference"><a href="#cite_note-a">[a]</a></sup></p>
<h2 id="Notes">Notes</h2><div class="reflist" data-mw-group="lower-alpha"><ol class="references" data-mw-group="lower-alpha"><li id="cite_note-a"><span class="mw-cite-backlink"><a href="#cite_ref-a">^</a></span> <span class="reference-text">The note text.</span></li></ol></div>
<h2 id="References">References</h2><div class="reflist"><ol class="references"><li id="cite_note-1"><span class="mw-cite-backlink"><a href="#cite_ref-1">^</a></span> <span class="reference-text">A source, 2020.</span></li></ol></div>
<h2>See also</h2><ul><li><a href="./Ticks" title="Ticks">Ticks</a></li><li><a rel="nofollow" class="external text" href="https://example.org">outside</a></li><li><a href="./Missing" class="new">red link</a></li></ul>''',
 'Boxes': '''<table class="infobox vcard"><tr><th colspan="2" class="infobox-above">Boxes</th></tr><tr><td colspan="2" class="infobox-image"><span typeof="mw:File"><a href="./File:B.jpg"><img src="../I/B.jpg.webp" width="220" height="150"></a></span><div class="infobox-caption">A box</div></td></tr><tr><th class="infobox-label">Size</th><td class="infobox-data">Big</td></tr></table>
<p>Text before.</p><div class="navbox" role="navigation"><div class="navbox-inner"><div>Nested <div>deeper</div> navigation</div></div></div><p>Text between.</p>
<figure typeof="mw:File/Thumb"><a href="./File:F.jpg"><img src="../I/F.jpg.webp" width="250" height="180"></a><figcaption>Figure caption</figcaption></figure>
<div class="thumb tright"><div class="thumbinner"><a href="./File:T.png"><img src="../I/T.png" width="200" height="100" class="thumbimage"></a><div class="thumbcaption"><div class="magnify"><a href="./File:T.png">m</a></div>Old style caption</div></div></div>
<ul class="gallery mw-gallery-traditional"><li class="gallerybox"><div class="thumb"><a href="./File:G1.jpg"><img src="../I/G1.jpg" width="120" height="90"></a></div><div class="gallerytext">Gallery one</div></li></ul>
<div class="thumb noprint"><div class="thumbinner"><img src="../I/N.png" width="100" height="100"></div></div>
<div class="noprint thumb"><img src="../I/N2.png" width="100" height="100"></div>
<table class="sidebar"><tr><td>Sidebar <b>bold</b></td></tr></table><p>End.</p>''',
 'Oddities': '''<P CLASS="lead">Upper case <B>tags</B> and <A HREF="./Code">links</A>.</P>
<p>Unquoted <a href=./Maths class=foo>attrs</a> and <a href='./Notes' title='It"s'>single</a>.</p>
<p>Literal < sign and a&lt;b and <unknown-tag attr="x">unknown</unknown-tag> tags <custom>here</custom>.</p>
<p>Self closing<br/>break<br />and <wbr>wbr<img src="../I/z.png"/>.</p>
<p>Split &am<span>p; entity</span> and &#x41; &nbsp;nbsp.</p>
<noscript><img src="../I/ns.png"></noscript><svg width="10"><text>svg text</text></svg>
<template><p>template</p></template><button>Press</button>
<div style="display:none">hidden div <div>inner</div></div><span class="mw-editsection">[edit]</span>
<blockquote><p>Quoted</p></blockquote><ol start="3"><li>three</li><li>four</li></ol>
<dl><dt>Term</dt><dd>Definition</dd></dl><p>Sub<sub>2</sub> sup<sup>3</sup> <small>small</small> <i>i</i> <em>em</em> <cite>cite</cite></p>
<table><caption>Cap</caption><tr><th rowspan="2">h</th><td>1</td></tr><tr><td>2</td></tr></table>
<div class="hatnote navigation-not-searchable">Not searchable</div><section data-mw-section-id="1"><h2>Section</h2><p>In section.</p></section>''',
 'Redirect_page': '',
}

with Creator(sys.argv[1]).config_indexing(False, 'eng').config_clustersize(64 * 1024) as c:
    c.set_mainpath('Ticks')
    for k, v in (('Name', 'slimtest'), ('Title', 'Slim test'), ('Language', 'eng'),
                 ('Creator', 'test'), ('Publisher', 'test'), ('Description', 'slim test'),
                 ('Date', '2026-11-16')):
        c.add_metadata(k, v)
    for path, body in PAGES.items():
        if path == 'Redirect_page':
            html = ('<!DOCTYPE html><html><head><meta charset="utf-8"><title>R</title>'
                    '<meta http-equiv="refresh" content="0;URL=\'./Notes#References\'"></head>'
                    '<body></body></html>')
        else:
            html = page(path.replace('_', ' '), body)
        c.add_item(Page(path, path.replace('_', ' '), html))
