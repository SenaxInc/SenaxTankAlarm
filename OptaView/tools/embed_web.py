#!/usr/bin/env python3
"""Turns web/index.html into the header that builds the page into the OptaView firmware.

    python3 OptaView/tools/embed_web.py              # rewrite OptaViewWebPage.h
    python3 OptaView/tools/embed_web.py --check      # fail if the header is out of date
    python3 OptaView/tools/embed_web.py --selftest   # check that the rules below refuse what they should

The header is generated: edit web/index.html, then run this script. The host test suite
(tests/host/optaview) runs it with --selftest and --check, so a stale header fails the pull
request.

The page is one file with one <style> and one <script>. The header also carries the page's
Content-Security-Policy, which names those two blocks by their hashes: the browser then runs
that style sheet and that script and nothing else, whatever ends up in the page. This script
therefore refuses a page that needs more than the policy allows (an inline event handler, a
style attribute, a style sheet it imports, a font, a script file, any file but a data: image),
because such a page would work when opened from disk and fail when the Opta serves it. It reads
the page the way a browser does where the two could differ: each tag attribute by attribute, with
character references decoded (&#32; is a space), tag names in any case, a block ending at the
first "</script" or "</style", and the escapes of the style sheet (@\\69mport is @import) and of
the script's names (\\u0065val is eval). Past that it reads the style sheet and the script as
text, so what the script puts together while it runs is left to the browser test, which runs the
page under the policy. The self-test adds one forbidden thing at a time to the page and checks
that it is refused, and that the page as it is passes.
"""

import base64
import hashlib
import html
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, "..", "web", "index.html")
HEADER = os.path.join(HERE, "..", "OptaViewWebPage.h")
DELIMITER = "OVWEB"
LINE_WIDTH = 110

# What the page must not contain, and why. The groups are matched against the markup, the style
# sheet and the script; the tags of the markup are also read one by one (markup_problem()).
MARKUP_RULES = [
    (r"javascript:", "a javascript: address"),
]
STYLE_RULES = [
    (r"@import\b", "an @import (the policy blocks the style sheet it brings in)"),
    (r"@font-face\b", "a web font (the policy allows none; use the fonts the system has)"),
    (r"\burl\((?!\s*[\"']?\s*data:)", "a file named by url() (the policy allows only data: images)"),
    (r"image-set\(", "an image-set() (the policy allows only data: images; use url(data:...))"),
]
SCRIPT_RULES = [
    (r"\b(?:inner|outer)HTML\b", "innerHTML (text from the controller goes in with textContent)"),
    (r"\binsertAdjacentHTML\b", "insertAdjacentHTML (text from the controller goes in with textContent)"),
    (r"\bdocument\.write", "document.write"),
    (r"\beval\s*\(", "eval (the policy blocks it)"),
    (r"\bnew\s+Function\b", "new Function (the policy blocks it)"),
    (r"setAttribute\(\s*[\"']style[\"']", "a style attribute (the policy blocks it; use a class)"),
    (r"(?:https?|wss?)://", "an address on another server"),
    (r"[\"'`]//[^/\s\"'`]", "an address on another server"),
    (r"javascript:", "a javascript: address"),
    (r"\bnew\s+(?:Shared)?Worker\b|\bimportScripts\b|\bimport\s*\(|\bserviceWorker\b"
     r"|createElement\(\s*[\"']script[\"']", "a script file (the policy runs this script and no other)"),
    (r"\bset(?:Timeout|Interval)\s*\(\s*[\"'`]", "a string run as code (the policy blocks it)"),
    (r"\.src(?:set)?\s*=(?!=)(?!\s*[\"'`]data:)"
     r"|setAttribute\(\s*[\"']src(?:set)?[\"']\s*,(?!\s*[\"'`]data:)",
     "a file named by the script (the policy allows only data: images)"),
    (r"createElement\(\s*[\"'](?:style|link|iframe|frame|object|embed|base)[\"']",
     "an element the policy blocks (made by the script)"),
]

# Elements the policy does not let the page use.
ELEMENTS_REFUSED = ("iframe", "frame", "frameset", "object", "embed", "applet", "base")
# The attributes that make the browser load a file, and those that load an image, which the
# policy lets come from a data: address. Any other file is blocked, data: or not: a style sheet,
# a sound, a video, its subtitles, a font, a script.
FILE_ATTRIBUTES = ("src", "srcset", "imagesrcset", "poster", "background", "href", "xlink:href")
IMAGE_ATTRIBUTES = {("img", "src"), ("img", "srcset"), ("source", "srcset"), ("input", "src"),
                    ("video", "poster"), ("image", "href"), ("image", "xlink:href"),
                    ("feimage", "href"), ("feimage", "xlink:href")}
ICON_LINKS = ("icon", "shortcut icon")
# How a browser reads a tag (start_tags()).
TAG_NAME = re.compile(r"[a-zA-Z][^\s/>]*")
ATTRIBUTE_NAME = re.compile(r"[^\s/>][^\s/>=]*")
EQUALS = re.compile(r"\s*=\s*")
UNQUOTED_VALUE = re.compile(r"[^\s>]*")
COMMENT_END = re.compile(r"--!?>")
# What a browser takes for a space between the parts of an attribute (srcset, rel), and what an
# address loses at both ends (the controls and the space) and inside (tabs and newlines).
HTML_SPACE = " \t\n\f\r"
URL_ENDS = "".join(chr(code) for code in range(0x21))
URL_DROPPED = re.compile(r"[\t\n\r]")
# The escapes the browser reads before the rules could see the words they spell.
CSS_ESCAPE = re.compile(r"\\(?:([0-9a-fA-F]{1,6})[ \t\n\f\r]?|(\n)|(.))", re.DOTALL)
JS_ESCAPE = re.compile(r"\\u(?:([0-9a-fA-F]{4})|\{([0-9a-fA-F]{1,6})\})")


def fail(message):
    raise SystemExit("web/index.html: " + message)


def only_block(page, tag):
    """The text between <tag> and </tag>. There must be exactly one of each in the page, in any
    case and anywhere, the start tag without attributes: a second <SCRIPT> would run without a
    hash in the policy, and the browser would block it. A browser ends the block at the first
    "</tag" followed by a space, "/" or ">", in any case, and so does this."""
    opened = re.findall(r"<%s\b[^>]*>?" % tag, page, re.IGNORECASE)
    if opened != ["<%s>" % tag]:
        fail("there must be exactly one <%s>, in lower case and without attributes; found %r"
             % (tag, opened))
    closed = re.findall(r"</%s[\s/>]" % tag, page, re.IGNORECASE)
    if closed != ["</%s>" % tag]:
        fail("there must be exactly one </%s>, in lower case and with nothing before its >; found %r"
             % (tag, closed))
    start = page.index("<%s>" % tag) + len(tag) + 2
    end = page.find("</%s>" % tag, start)
    if end < 0:
        fail("the </%s> must come after the <%s>" % (tag, tag))
    return page[start:end], start, end


def css_unescaped(text):
    """The style sheet with each escape replaced by what it stands for, as a browser reads it."""
    def one(match):
        if match.group(1):
            code = int(match.group(1), 16)
            return chr(code) if 0 < code <= 0x10FFFF and not 0xD800 <= code <= 0xDFFF else "\ufffd"
        return "" if match.group(2) else match.group(3)  # an escaped newline continues a string
    return CSS_ESCAPE.sub(one, text)


def js_unescaped(text):
    """The script with each \\u escape replaced by its character, as in a name it stands for it."""
    def one(match):
        code = int(match.group(1) or match.group(2), 16)
        return chr(code) if code <= 0x10FFFF and not 0xD800 <= code <= 0xDFFF else "\ufffd"
    return JS_ESCAPE.sub(one, text)


def url_of(value):
    """An attribute's address as a browser parses it: without the controls and spaces at its
    ends, and without the tabs and newlines inside it."""
    return URL_DROPPED.sub("", value).strip(URL_ENDS)


def start_tags(markup):
    """Each start tag in the markup as a browser reads it: its name and its attributes in lower
    case, each value without its quotes and with its character references decoded, and the tag's
    text. A quoted value may hold ">" and "<". Comments are skipped; a comment ends at the first
    "-->" or "--!>", "<!-->" included."""
    at = 0
    while True:
        at = markup.find("<", at)
        if at < 0:
            return
        if markup.startswith("<!--", at):
            end = COMMENT_END.search(markup, at + 2)
            at = end.end() if end else len(markup)
            continue
        name = TAG_NAME.match(markup, at + 1)
        if not name:
            at += 1
            continue
        start = at
        at = name.end()
        attributes = []
        while True:
            while at < len(markup) and (markup[at].isspace() or markup[at] == "/"):
                at += 1
            if at >= len(markup) or markup[at] == ">":
                break
            attribute = ATTRIBUTE_NAME.match(markup, at)
            at = attribute.end()
            value = ""
            equals = EQUALS.match(markup, at)
            if equals:
                at = equals.end()
                if markup[at:at + 1] in ("\"", "'"):
                    end = markup.find(markup[at], at + 1)
                    end = len(markup) if end < 0 else end
                    value = markup[at + 1:end]
                    at = end + 1
                else:
                    unquoted = UNQUOTED_VALUE.match(markup, at)
                    value = unquoted.group(0)
                    at = unquoted.end()
            attributes.append((attribute.group(0).lower(), html.unescape(value)))
        yield name.group(0).lower(), attributes, markup[start:at + 1]
        at += 1


def srcset_addresses(value):
    """The addresses in a srcset, by the HTML rules: an address runs to the next space (a tab, a
    newline, a form feed or a carriage return too), and a comma at its end or after its sizes
    ends that candidate."""
    addresses = []
    rest = value
    while True:
        rest = rest.lstrip(HTML_SPACE + ",")
        if not rest:
            return addresses
        address = re.match(r"[^ \t\n\f\r]*", rest).group(0)
        rest = rest[len(address):]
        if address.endswith(","):
            addresses.append(address.rstrip(","))
        else:
            addresses.append(address)
            rest = rest.split(",", 1)[1] if "," in rest else ""


def markup_problem(markup):
    """The first thing in the tags of the markup that the policy blocks, as (what, the tag)."""
    for name, attributes, text in start_tags(markup):
        if name in ELEMENTS_REFUSED:
            return "an element the policy does not allow", text
        rel = next((" ".join(re.findall(r"[^ \t\n\f\r]+", value.lower())) for key, value in attributes
                    if key == "rel"), "")
        for key, value in attributes:
            if re.match(r"on[a-z]+$", key):
                return "an inline event handler (the policy blocks it; use addEventListener)", text
            if key == "style":
                return "a style attribute (the policy blocks it; use a class)", text
            if key in ("action", "formaction"):
                return "a form action (the policy sends no form; the script sends it)", text
            if key == "ping":
                return "a ping (the policy lets the page send only what its script sends)", text
            address = url_of(value)
            if "javascript:" in address.lower():
                return "a javascript: address", text
            if key not in FILE_ATTRIBUTES:
                continue
            if re.match(r"(?:https?:)?//", address, re.IGNORECASE):
                return "a file from another server", text
            # A link to another page, or to a part of this one, loads nothing.
            if key in ("href", "xlink:href"):
                if name in ("a", "area") or (address.startswith("#") and name not in ("link", "image")):
                    continue
            image = ((name, key) in IMAGE_ATTRIBUTES or key == "background"
                     or (name == "link" and rel in ICON_LINKS))
            if key.endswith("srcset"):
                addresses = [url_of(one) for one in srcset_addresses(value)]
            else:
                addresses = [address]
            for each in addresses:
                if not (image and each[:5].lower() == "data:"):
                    return "a file the policy blocks (it allows only data: images)", text
    return None


def csp_hash(text):
    return "'sha256-%s'" % base64.b64encode(hashlib.sha256(text.encode("ascii")).digest()).decode("ascii")


def chunks(text, width):
    """Splits a line into pieces no longer than width."""
    if not text:
        return
    start = 0
    while start < len(text):
        yield text[start:start + width]
        start += width


def read_page(path):
    # Universal newlines: a checkout with CR LF gives the same page, and the same hashes.
    with open(path, "r", encoding="ascii", errors="strict", newline=None) as handle:
        try:
            page = handle.read()
        except UnicodeDecodeError as error:
            fail("it must be plain ASCII (write other characters as entities or \\u escapes): %s" % error)
    for number, line in enumerate(page.split("\n"), 1):
        for char in line:
            if ord(char) < 0x20 or ord(char) == 0x7F:
                fail("line %d holds a control character (tabs included): use spaces" % number)
        if line != line.rstrip():
            fail("line %d ends in spaces" % number)
    if not page.endswith("\n") or page.endswith("\n\n"):
        fail("it must end with exactly one newline")
    return page


def render(page_path):
    page = read_page(page_path)
    if ")" + DELIMITER + '"' in page:
        fail("it contains the raw string delimiter; change DELIMITER in embed_web.py")
    if "??" in page:
        fail('it contains "??", which older compilers read as the start of a trigraph')

    style, style_start, style_end = only_block(page, "style")
    script, script_start, script_end = only_block(page, "script")
    if not style_end < script_start:
        fail("the <style> must come before the <script>")
    markup = page[:style_start] + page[style_end:script_start] + page[script_end:]
    for pattern, what in MARKUP_RULES:
        found = re.search(pattern, markup, re.IGNORECASE)
        if found:
            fail("the markup has %s: %r" % (what, found.group(0)[:80]))
    problem = markup_problem(markup)
    if problem:
        fail("the markup has %s: %r" % (problem[0], problem[1][:80]))
    for pattern, what in STYLE_RULES:
        for text in (style, css_unescaped(style)):
            found = re.search(pattern, text, re.IGNORECASE)
            if found:
                fail("the style sheet has %s: %r" % (what, found.group(0)[:80]))
    for pattern, what in SCRIPT_RULES:
        for text in (script, js_unescaped(script)):
            found = re.search(pattern, text)
            if found:
                fail("the script has %s: %r" % (what, found.group(0)[:80]))

    policy = "; ".join([
        "default-src 'none'",
        "script-src " + csp_hash(script),
        "style-src " + csp_hash(style),
        "connect-src 'self'",
        "img-src data:",
        "form-action 'none'",
        "base-uri 'none'",
        "frame-ancestors 'none'",
    ])

    lines = [
        "/**",
        " * OptaViewWebPage.h",
        " *",
        " * GENERATED by tools/embed_web.py from web/index.html. Do not edit by hand: change the page",
        " * and run the script again.",
        " */",
        "",
        "#ifndef OPTAVIEW_WEB_PAGE_H",
        "#define OPTAVIEW_WEB_PAGE_H",
        "",
        "#include <stddef.h>",
        "",
        "namespace optaview {",
        "",
        "// The page: %d bytes, its lines ending in LF." % len(page),
        "static const char OV_WEB_PAGE[] =",
    ]
    # One raw string literal per piece of a line; the compiler joins adjacent literals.
    for line in page.split("\n")[:-1]:
        pieces = ['R"%s(%s)%s"' % (DELIMITER, piece, DELIMITER) for piece in chunks(line, LINE_WIDTH)]
        if not pieces:
            lines.append('    "\\n"')
            continue
        for piece in pieces[:-1]:
            lines.append("    " + piece)
        lines.append("    " + pieces[-1] + ' "\\n"')
    lines[-1] += ";"
    lines += [
        "static const size_t OV_WEB_PAGE_LENGTH = sizeof(OV_WEB_PAGE) - 1;",
        "",
        "// Sent with the page. The two hashes are those of its <style> and its <script>: the browser",
        "// applies that style sheet and runs that script, and nothing else.",
        "static const char OV_WEB_PAGE_HEADERS[] =",
        '    "Content-Security-Policy: "',
    ]
    for part in policy.split("; "):
        lines.append('    "%s; "' % part)
    lines[-1] = lines[-1][:-3] + '\\r\\n";'
    lines += [
        "",
        "}  // namespace optaview",
        "",
        "#endif  // OPTAVIEW_WEB_PAGE_H",
        "",
    ]
    return "\n".join(lines)


# The self-test: each entry is added to the page in one place (after <style>, before </body> or
# after <script>) and must be refused with the words given, or, in the second list, accepted. In
# the third list each entry changes the page's own tags, one replacement after the other, and
# must be refused.
SELFTEST_REFUSED = [
    ("style", '@import url("/theme.css");', "an @import"),
    ("style", "@IMPORT 'theme.css';", "an @import"),
    ("style", "@font-face { font-family: f; src: url(data:font/woff2;base64,AAAA); }", "a web font"),
    ("style", ".x { background: url( 'logo.png' ); }", "named by url()"),
    ("style", ".x { background: URL(//elsewhere.example/a.png); }", "named by url()"),
    ("style", '.x { background-image: -webkit-image-set("logo.png" 1x); }', "an image-set()"),
    # The browser reads a CSS escape as the letter it stands for.
    ("style", '@\\69mport "theme.css";', "an @import"),
    ("style", '@\\69 mport "theme.css";', "an @import"),
    ("style", '@\\000069mport "theme.css";', "an @import"),
    ("style", ".x { background: \\75rl(logo.png); }", "named by url()"),
    ("style", "@\\66ont-face { font-family: f; src: url(data:font/woff2;base64,AAAA); }", "a web font"),
    ("style", '@\\import "theme.css";', "an @import"),
    ("style", ".x { background: u\\rl(logo.png); }", "named by url()"),
    ("markup", '<link rel="stylesheet" href="theme.css">', "a file the policy blocks"),
    ("markup", '<img src="logo.png" alt="">', "a file the policy blocks"),
    ("markup", '<img alt="" srcset="logo.png 2x">', "a file the policy blocks"),
    ("markup", '<video poster="frame.png"></video>', "a file the policy blocks"),
    ("markup", '<form action="/login"></form>', "a form action"),
    ("markup", '<button formaction="/login">Go</button>', "a form action"),
    ("markup", '<button onclick="go()">Go</button>', "an inline event handler"),
    ("markup", '<p style="color: red">x</p>', "a style attribute"),
    ("markup", '<a href="javascript:go()">x</a>', "a javascript: address"),
    ("markup", '<a href="https://elsewhere.example/">x</a>', "a file from another server"),
    ("markup", '<a href="#live" ping="/count">x</a>', "a ping"),
    # A second block, in any case, would run without a hash in the policy.
    ("markup", "<SCRIPT>go()</SCRIPT>", "there must be exactly one <script>"),
    ("markup", "<Script>go()</Script>", "there must be exactly one <script>"),
    ("markup", '<script src="more.js"></script>', "there must be exactly one <script>"),
    ("markup", "<STYLE>p { color: red; }</STYLE>", "there must be exactly one <style>"),
    # A value is read with its character references decoded, and an address without the tabs and
    # newlines inside it.
    ("markup", '<a href="java&#115;cript:go()">x</a>', "a javascript: address"),
    ("markup", '<a href="java&#9;script:go()">x</a>', "a javascript: address"),
    ("markup", '<a href="java\nscript:go()">x</a>', "a javascript: address"),
    ("markup", '<a href="java&#13;script:go()">x</a>', "a javascript: address"),
    ("markup", '<a href="Java&#83;cript:go()">x</a>', "a javascript: address"),
    ("markup", '<svg><a><set attributeName="href" to="java&#115;cript:go()"/></a></svg>',
     "a javascript: address"),
    ("markup", '<img alt="" srcset="data:image/png;base64,AAAA&#32;1x,&#32;logo.png&#32;2x">',
     "a file the policy blocks"),
    ("markup", '<img alt="" srcset="data:image/png;base64,AAAA&#9;1x,&#9;logo.png&#9;2x">',
     "a file the policy blocks"),
    ("markup", '<img alt="" srcset="data:image/png;base64,AAAA&#12;1x,&#13;logo.png&#10;2x">',
     "a file the policy blocks"),
    ("markup", '<img src="&#32;logo.png" alt="">', "a file the policy blocks"),
    ("markup", '<img src="&#32;//elsewhere.example/a.png" alt="">', "a file from another server"),
    # A no-break space is not one of the spaces an address loses at its ends.
    ("markup", '<img src="&nbsp;data:image/png;base64,AAAA" alt="">', "a file the policy blocks"),
    ("markup", "<iframe></iframe>", "an element the policy does not allow"),
    ("markup", "<IFRAME></IFRAME>", "an element the policy does not allow"),
    ("markup", "<p class=x><iframe></iframe>", "an element the policy does not allow"),
    ("markup", '<object data="x.svg"></object>', "an element the policy does not allow"),
    ("markup", '<embed src="x.svg">', "an element the policy does not allow"),
    ("markup", '<base href="/">', "an element the policy does not allow"),
    ("markup", '<frame src="x.html">', "an element the policy does not allow"),
    ("markup", "<frameset></frameset>", "an element the policy does not allow"),
    ("markup", '<applet code="x"></applet>', "an element the policy does not allow"),
    # A data: address is allowed only where the policy allows it: in an image.
    ("markup", '<link rel="stylesheet" href="data:text/css,p{color:red}">', "a file the policy blocks"),
    ("markup", '<link rel="preload" href="data:,">', "a file the policy blocks"),
    ("markup", '<link rel="icon" href="#top">', "a file the policy blocks"),
    ("markup", '<link rel="preload" as="image" imagesrcset="data:,">', "a file the policy blocks"),
    ("markup", '<audio src="data:audio/wav;base64,AAAA"></audio>', "a file the policy blocks"),
    ("markup", '<video><source src="data:video/mp4;base64,AAAA"></video>', "a file the policy blocks"),
    ("markup", '<img alt="" srcset="data:image/png;base64,A 1x, logo.png 2x">', "a file the policy blocks"),
    ("markup", '<img alt="" srcset="data:image/png;base64,AAAA, logo.png">', "a file the policy blocks"),
    ("markup", '<svg><image href="logo.png"/></svg>', "a file the policy blocks"),
    ("markup", '<svg><image xlink:href="#top"/></svg>', "a file the policy blocks"),
    ("markup", '<svg><use href="icons.svg#a"/></svg>', "a file the policy blocks"),
    ("markup", '<table background="paper.png"></table>', "a file the policy blocks"),
    ("markup", '<IMG SRC="logo.png" ALT="">', "a file the policy blocks"),
    ("markup", '<img/src="logo.png" alt="">', "a file the policy blocks"),
    ("markup", '<img src="HTTPS://elsewhere.example/a.png" alt="">', "a file from another server"),
    ("markup", "<img src=//elsewhere.example/a.png alt=x>", "a file from another server"),
    # A quoted value may hold ">": what comes after it is still in the tag.
    ("markup", '<p title="a > b" onclick="go()">x</p>', "an inline event handler"),
    ("markup", "<p title='a > b' style=\"color: red\">x</p>", "a style attribute"),
    ("markup", '<img alt="a > b" src="logo.png">', "a file the policy blocks"),
    # A comment ends where a browser ends it.
    ("markup", '<!-- <b title="a --> <img src="logo.png" alt="">', "a file the policy blocks"),
    ("markup", '<!--> <img src="logo.png" alt=""> -->', "a file the policy blocks"),
    ("markup", '<!-- a --!> <img src="logo.png" alt="">', "a file the policy blocks"),
    ("script", "x.innerHTML = y;", "innerHTML"),
    ("script", "x.inner\\u0048TML = y;", "innerHTML"),
    ("script", "\\u0065val(y);", "eval"),
    ("script", "\\u{65}val(y);", "eval"),
    ("script", "var end = '</script>';", "there must be exactly one </script>"),
    ("script", "var end = '</SCRIPT ';", "there must be exactly one </script>"),
    ("script", "var end = '</script/';", "there must be exactly one </script>"),
    ("script", "x.insertAdjacentHTML('beforeend', y);", "insertAdjacentHTML"),
    ("script", "document.write(y);", "document.write"),
    ("script", "eval(y);", "eval"),
    ("script", "new Function(y);", "new Function"),
    ("script", "x.setAttribute('style', y);", "a style attribute"),
    ("script", "fetch('https://elsewhere.example/');", "an address on another server"),
    ("script", "new WebSocket('wss://elsewhere.example/');", "an address on another server"),
    ("script", "fetch('//elsewhere.example/');", "an address on another server"),
    ("script", "location = 'javascript:go()';", "a javascript: address"),
    ("script", "new Worker('w.js');", "a script file"),
    ("script", "new SharedWorker('w.js');", "a script file"),
    ("script", "importScripts('w.js');", "a script file"),
    ("script", "import('./more.js');", "a script file"),
    ("script", "navigator.serviceWorker.register('/sw.js');", "a script file"),
    ("script", "document.createElement('script');", "a script file"),
    ("script", "setTimeout('go()', 10);", "a string run as code"),
    ("script", 'setInterval("go()", 10);', "a string run as code"),
    ("script", "image.src = 'logo.png';", "a file named by the script"),
    ("script", "image.srcset = name;", "a file named by the script"),
    ("script", "image.setAttribute('src', name);", "a file named by the script"),
    ("script", "document.createElement('style');", "an element the policy blocks"),
    ("script", "document.createElement('link');", "an element the policy blocks"),
    ("script", "document.createElement('iframe');", "an element the policy blocks"),
    ("script", "document.createElement('frame');", "an element the policy blocks"),
    ("script", "document.createElement('object');", "an element the policy blocks"),
    ("script", "document.createElement('embed');", "an element the policy blocks"),
    ("script", "document.createElement('base');", "an element the policy blocks"),
]
SELFTEST_ALLOWED = [
    ("style", ".x { background: url(data:image/png;base64,AAAA); }"),
    ("style", '.x { background: url( "data:image/svg+xml,%3Csvg%3E" ); }'),
    ("markup", '<img src = "data:image/png;base64,AAAA" alt="">'),
    ("markup", '<img src="Data:image/png;base64,AAAA" alt="">'),
    ("markup", '<a href="#live">x</a>'),
    ("markup", '<a href="/snapshot">x</a>'),
    ("markup", '<link rel="Shortcut  Icon" href="data:,">'),
    ("markup", '<img alt="" srcset="data:image/png;base64,AAAA 1x, data:image/png;base64,BBBB 2x">'),
    ("markup", '<picture><source srcset="data:image/png;base64,AAAA"><img src="data:,a" alt=""></picture>'),
    ("markup", '<input type="image" src="data:image/png;base64,AAAA" alt="Go">'),
    ("markup", '<video poster="data:image/png;base64,AAAA"></video>'),
    ("markup", '<table background="data:image/png;base64,AAAA"></table>'),
    ("markup", '<svg><image href="data:image/png;base64,AAAA"/><image xlink:href="data:,"/></svg>'),
    ("markup", '<svg><filter><feImage href="data:,"/><feImage xlink:href="data:,a"/></filter></svg>'),
    ("markup", '<svg><use href="#f"/><a xlink:href="/snapshot">x</a></svg>'),
    ("markup", '<p data-src="logo.png" title="a > b">x</p>'),
    ("markup", '<!-- <img src="logo.png"> -->'),
    ("markup", '<img src="data&#58;image/png,x" alt="">'),
    ("markup", '<img src="&#32;data:image/png,x&#10;" alt="">'),
    ("markup", '<img alt="" srcset="data:image/png;base64,AAAA&#9;1x,&#9;data:,b&#9;2x">'),
    ("markup", '<link rel="shortcut&#9;icon" href="data:,">'),
    ("markup", '<img alt="" srcset="data:,a 1x,&#12;data:,b 2x">'),
    ("script", "image.src = 'data:image/png;base64,AAAA';"),
    ("script", "image.setAttribute('src', 'data:image/png;base64,AAAA');"),
    ("script", "if (image.src == other) { setTimeout(go, 10); } // see //notes"),
]
SELFTEST_REPLACED = [
    ([("<script>", "<SCRIPT>")], "there must be exactly one <script>"),
    ([("<script>", '<script type="module">')], "there must be exactly one <script>"),
    ([("</script>", "</SCRIPT>")], "there must be exactly one </script>"),
    ([("</script>", "</script >")], "there must be exactly one </script>"),
    ([("</script>", "</script/>")], "there must be exactly one </script>"),
    ([("<style>", "<Style>")], "there must be exactly one <style>"),
    ([("</style>", "</style >")], "there must be exactly one </style>"),
    ([("</script>", ""), ("<script>", "</script><script>")], "the </script> must come after the <script>"),
]


def selftest():
    page = read_page(PAGE)
    anchors = {"style": ("<style>", True), "markup": ("</body>", False), "script": ("<script>", True)}

    def variant(where, text):
        anchor, after = anchors[where]
        at = page.index(anchor) + (len(anchor) if after else 0)
        return page[:at] + "\n" + text + "\n" + page[at:]

    def outcome(text):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "index.html")
            with open(path, "w", encoding="ascii", newline="\n") as handle:
                handle.write(text)
            try:
                render(path)
            except SystemExit as refusal:
                return str(refusal.code)
        return None

    problems = []
    got = outcome(page)
    if got is not None:
        problems.append("the page as it is: %s" % got)
    for where, text, words in SELFTEST_REFUSED:
        got = outcome(variant(where, text))
        if got is None or words not in got:
            problems.append("%s %r: %s" % (where, text, got or "accepted"))
    for where, text in SELFTEST_ALLOWED:
        got = outcome(variant(where, text))
        if got is not None:
            problems.append("%s %r: %s" % (where, text, got))
    for changes, words in SELFTEST_REPLACED:
        changed = page
        for old, new in changes:
            if changed.count(old) != 1:
                problems.append("%r is not in the page once" % old)
                break
            changed = changed.replace(old, new)
        else:
            got = outcome(changed)
            if got is None or words not in got:
                problems.append("%r: %s" % (changes, got or "accepted"))
    for problem in problems:
        print("self-test: " + problem)
    total = 1 + len(SELFTEST_REFUSED) + len(SELFTEST_ALLOWED) + len(SELFTEST_REPLACED)
    if problems:
        print("embed_web.py self-test: %d of %d pages not as expected" % (len(problems), total))
        return 1
    print("embed_web.py self-test: all %d pages as expected" % total)
    return 0


def main(argv):
    if "--selftest" in argv:
        return selftest()
    check = "--check" in argv
    text = render(PAGE)
    if check:
        try:
            with open(HEADER, "r", encoding="utf-8", newline=None) as handle:
                current = handle.read()
        except OSError:
            current = None
        if current != text:
            print("OptaViewWebPage.h is out of date. Run: python3 OptaView/tools/embed_web.py")
            return 1
        print("OptaViewWebPage.h matches web/index.html")
        return 0
    with open(HEADER, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("wrote %s (%d bytes)" % (os.path.normpath(HEADER), len(text)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
