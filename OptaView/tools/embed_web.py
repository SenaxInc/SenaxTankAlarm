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
style attribute, a style sheet it imports, a font, any file but a data: image), because such a
page would work when opened from disk and fail when the Opta serves it. The self-test adds one
such thing at a time to the page and checks that it is refused, and that the page as it is
passes.
"""

import base64
import hashlib
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
# sheet and the script.
MARKUP_RULES = [
    (r"<[^>]*\son[a-z]+\s*=", "an inline event handler (the policy blocks it; use addEventListener)"),
    (r"<[^>]*\sstyle\s*=", "a style attribute (the policy blocks it; use a class)"),
    (r"javascript:", "a javascript: address"),
    (r"<[^>]*\s(?:src|href|action)\s*=\s*[\"']?(?:https?:)?//", "a file from another server"),
    (r"<(?:iframe|object|embed|base)\b", "an element the policy does not allow"),
    (r"<[^>]*\s(?:src|srcset|poster)\s*=(?!\s*[\"']?data:)",
     "a file the policy blocks (it allows only data: images)"),
    (r"<link\b[^>]*\shref\s*=(?!\s*[\"']?data:)", "a file the policy blocks (it allows only data: images)"),
    (r"<[^>]*\s(?:form)?action\s*=", "a form action (the policy sends no form; the script sends it)"),
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
    (r"https?://", "an address on another server"),
]


def fail(message):
    raise SystemExit("web/index.html: " + message)


def only_block(page, tag):
    """The text between <tag> and </tag>. There must be exactly one, and without attributes."""
    opened = re.findall(r"<%s\b[^>]*>" % tag, page)
    if opened != ["<%s>" % tag]:
        fail("there must be exactly one <%s>, without attributes; found %r" % (tag, opened))
    start = page.index("<%s>" % tag) + len(tag) + 2
    end = page.find("</%s>" % tag, start)
    if end < 0 or page.count("</%s>" % tag) != 1:
        fail("there must be exactly one </%s>" % tag)
    return page[start:end], start, end


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
    for pattern, what in STYLE_RULES:
        found = re.search(pattern, style, re.IGNORECASE)
        if found:
            fail("the style sheet has %s: %r" % (what, found.group(0)[:80]))
    for pattern, what in SCRIPT_RULES:
        found = re.search(pattern, script)
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
# after <script>) and must be refused with the words given, or, in the second list, accepted.
SELFTEST_REFUSED = [
    ("style", '@import url("/theme.css");', "an @import"),
    ("style", "@IMPORT 'theme.css';", "an @import"),
    ("style", "@font-face { font-family: f; src: url(data:font/woff2;base64,AAAA); }", "a web font"),
    ("style", ".x { background: url( 'logo.png' ); }", "named by url()"),
    ("style", ".x { background: URL(//elsewhere.example/a.png); }", "named by url()"),
    ("style", '.x { background-image: -webkit-image-set("logo.png" 1x); }', "an image-set()"),
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
    ("markup", "<iframe></iframe>", "an element the policy does not allow"),
    ("script", "x.innerHTML = y;", "innerHTML"),
    ("script", "x.insertAdjacentHTML('beforeend', y);", "insertAdjacentHTML"),
    ("script", "document.write(y);", "document.write"),
    ("script", "eval(y);", "eval"),
    ("script", "new Function(y);", "new Function"),
    ("script", "x.setAttribute('style', y);", "a style attribute"),
    ("script", "fetch('https://elsewhere.example/');", "an address on another server"),
]
SELFTEST_ALLOWED = [
    ("style", ".x { background: url(data:image/png;base64,AAAA); }"),
    ("style", '.x { background: url( "data:image/svg+xml,%3Csvg%3E" ); }'),
    ("markup", '<img src = "data:image/png;base64,AAAA" alt="">'),
    ("markup", '<a href="#live">x</a>'),
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
    for problem in problems:
        print("self-test: " + problem)
    total = 1 + len(SELFTEST_REFUSED) + len(SELFTEST_ALLOWED)
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
