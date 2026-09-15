#!/usr/bin/env python3
"""Static checks for the web pages embedded as PROGMEM raw strings in the Server and Viewer sketches.

Checks, per page:
  1. every inline <script> block passes `node --check`;
  2. every function called from an inline event handler attribute (on<event>="...") resolves to a
     function the page actually exposes at load time: either a `function name(` declared at the top
     level of a script block, or a `window.name = ...` assignment that executes when the page loads.
     Every bare call in the attribute value is checked (`onclick="a(); b()"` checks both), not just
     the first. Pages wrap their scripts in an async IIFE, so a plain `function name(){}` inside that
     closure is NOT reachable from an inline attribute (it throws ReferenceError at click time and
     nothing else reports it). A `window.name = ...` counts only when every enclosing function body
     is executed on load: an immediately-invoked function/arrow, or a `load`/`DOMContentLoaded`
     listener body. An assignment inside a function declaration, an ordinary callback, or an
     expression-bodied arrow (`.then(() => window.x = x)`) does not count.

The scanner tokenises the script (strings, template literals with nested `${}`, comments, regex
literals after punctuation or after keywords such as `return`) so braces inside those do not
affect the depth count. It is deliberately conservative: unusual constructs may produce a false
failure, never a silent pass.

Usage:  python TankAlarm-112025-Server-BluesOpta/check_web_pages.py [--quiet] [--allow-no-node]
        python TankAlarm-112025-Server-BluesOpta/check_web_pages.py --selftest
Exit status is non-zero on any failure, on a missing sketch, on a sketch that yields no pages, and
(unless --allow-no-node is given) when `node` is not on PATH.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SKETCHES = [
    os.path.join(ROOT, 'TankAlarm-112025-Server-BluesOpta', 'TankAlarm-112025-Server-BluesOpta.ino'),
    os.path.join(ROOT, 'TankAlarm-112025-Viewer-BluesOpta', 'TankAlarm-112025-Viewer-BluesOpta.ino'),
]
PAGE_RE = re.compile(r'const char (\w+)\[\] PROGMEM\s*=\s*R"HTML\(')
SCRIPT_RE = re.compile(r'<script(?![^>]*\bsrc=)[^>]*>(.*?)</script>', re.S)
# any on<event> attribute, case-insensitive (HTML attribute names are), whitespace around '=' allowed,
# quoted or unquoted value; a quoted value may sit inside a JS string, where the quotes are
# backslash-escaped
ATTR_RE = re.compile(r'(?<![\w$.])on[a-z]+\s*=\s*(?:(\\?)(["\'])(.*?)\2|([^\s>"\'=<`]+))', re.S | re.I)
# every bare call in an attribute value: name( not preceded by an identifier char or '.', plus window.name(
CALL_RE = re.compile(r'(?<![\w$.])([A-Za-z_$][\w$]*)\s*\(')
WINCALL_RE = re.compile(r'\bwindow\s*\.\s*([A-Za-z_$][\w$]*)\s*\(')
# callable browser globals only (window.location is an object, so `location()` would throw)
BROWSER_GLOBALS = {'fetch', 'alert', 'confirm', 'prompt', 'open', 'print', 'setTimeout', 'clearTimeout',
                   'encodeURIComponent', 'decodeURIComponent', 'parseInt', 'parseFloat', 'Number', 'String',
                   'Boolean', 'requestAnimationFrame', 'scrollTo'}
# names the call regex can capture that are syntax, not handler functions (e.g. onclick="if(x)f()")
RESERVED = {'if', 'for', 'while', 'switch', 'return', 'typeof', 'new', 'function', 'void', 'delete', 'throw',
            'case', 'do', 'else', 'try', 'catch', 'with', 'in', 'of', 'instanceof', 'yield', 'await', 'this',
            'super', 'class', 'let', 'const', 'var'}
IGNORE_NAMES = BROWSER_GLOBALS | RESERVED
# a '/' after one of these words starts a regex literal, not a division
REGEX_AFTER_WORDS = {'return', 'typeof', 'instanceof', 'in', 'of', 'new', 'delete', 'void', 'throw', 'case',
                     'do', 'else', 'yield', 'await'}
REGEX_AFTER_PUNCT = set('(,=:[!&|?{};+-*%<>~^')
BLOCK_WORDS = {'if', 'for', 'while', 'switch', 'catch', 'with'}
LISTENER_ARROW_RE = re.compile(
    r'addEventListener\(\s*[\'"](?:load|DOMContentLoaded)[\'"]\s*,\s*(?:async\s*)?(?:\([^()]*\)|[A-Za-z_$][\w$]*)\s*=>\s*$')
LISTENER_FUNC_RE = re.compile(
    r'addEventListener\(\s*[\'"](?:load|DOMContentLoaded)[\'"]\s*,\s*(?:async\s+)?function\s*[\w$]*\s*\([^()]*\)\s*$')
IDENT_START = re.compile(r'[A-Za-z_$]')
IDENT_RE = re.compile(r'[A-Za-z_$][\w$]*')


def extract_pages(path):
    """Return {name: joined_page_text} for every PROGMEM raw-string constant in a sketch."""
    text = open(path, encoding='utf-8', errors='replace').read()
    pages = {}
    for m in PAGE_RE.finditer(text):
        pos = m.end()
        segs = []
        while True:
            end = text.find(')HTML"', pos)
            if end < 0:
                break
            segs.append(text[pos:end])
            pos = end + len(')HTML"')
            nxt = re.match(r'\s*R"HTML\(', text[pos:])
            if not nxt:
                break
            pos += nxt.end()
        pages[m.group(1)] = ''.join(segs)
    return pages


def _skip_string(s, i):
    """Index just past the string literal starting at s[i] (a quote or backtick); nested `${}` in a
    template literal are skipped with _skip_splice."""
    q = s[i]
    i += 1
    n = len(s)
    while i < n and s[i] != q:
        if s[i] == '\\':
            i += 2
            continue
        if q == '`' and s.startswith('${', i):
            i = _skip_splice(s, i)
            continue
        i += 1
    return i + 1


def _skip_regex(s, i):
    """Index just past the regex literal starting at s[i] == '/'."""
    i += 1
    n = len(s)
    in_class = False
    while i < n:
        c = s[i]
        if c == '\\':
            i += 2
            continue
        if c == '[':
            in_class = True
        elif c == ']':
            in_class = False
        elif c == '/' and not in_class:
            break
        elif c == '\n':
            break
        i += 1
    i += 1
    while i < n and s[i].isalpha():
        i += 1
    return i


def _skip_splice(s, i):
    """Index just past the `${ ... }` starting at s[i], honouring strings, templates, comments,
    regex literals and nested braces inside the expression."""
    depth, i, n = 1, i + 2, len(s)
    last = '{'                  # last significant character, to tell a regex from a division
    while i < n and depth:
        c = s[i]
        if c in ' \t\r\n':
            i += 1
            continue
        if c in '"\'`':
            i = _skip_string(s, i)
            last = c
            continue
        if s.startswith('//', i):
            j = s.find('\n', i)
            i = n if j < 0 else j + 1
            continue
        if s.startswith('/*', i):
            j = s.find('*/', i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == '/' and last in REGEX_AFTER_PUNCT:
            i = _skip_regex(s, i)
            last = '/'
            continue
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
        last = c
        i += 1
    return i


def _strip_template_splices(value):
    """Remove `${ ... }` splices from an attribute value that sits inside a JS template literal:
    those expressions run at render time in the closure, not at click time. Strings, comments and
    nested templates inside a splice are lexed so a `{` in a string cannot swallow the rest."""
    out, i, n = [], 0, len(value)
    while i < n:
        if value.startswith('${', i):
            i = _skip_splice(value, i)
            continue
        out.append(value[i])
        i += 1
    return ''.join(out)


def _strip_js_literals(code):
    """Blank out string literals, template literals, comments and regex literals in a snippet of
    JavaScript so that call detection only sees executable code (`alert('missing()')` calls
    alert, not missing)."""
    out, i, n = [], 0, len(code)
    last = '('
    while i < n:
        c = code[i]
        if c in '"\'`':
            j = _skip_string(code, i)
            out.append(' ' * (j - i))
            i = j
            last = c
            continue
        if code.startswith('//', i):
            j = code.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
            continue
        if code.startswith('/*', i):
            j = code.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(' ' * (j - i))
            i = j
            continue
        if c == '/' and last in REGEX_AFTER_PUNCT:
            j = _skip_regex(code, i)
            out.append(' ' * (j - i))
            i = j
            last = '/'
            continue
        out.append(c)
        if c not in ' \t\r\n':
            last = c
        i += 1
    return ''.join(out)


def handler_calls(page):
    """Names called from inline on<event> attributes anywhere in the page text."""
    names = set()
    for m in ATTR_RE.finditer(page):
        value = m.group(3) if m.group(3) is not None else m.group(4)
        value = _strip_js_literals(_strip_template_splices(value.replace('\\"', '"').replace("\\'", "'")))
        names.update(CALL_RE.findall(value))
        names.update(WINCALL_RE.findall(value))
    return names


def _prev_nonspace(js, i):
    """Index of the last non-whitespace character before i, or -1."""
    j = i - 1
    while j >= 0 and js[j] in ' \t\r\n':
        j -= 1
    return j


def _matching_open_paren(js, close_idx):
    """Index of the '(' matching js[close_idx] == ')', scanning backwards (strings in parameter
    lists are rare; a mismatch only makes the brace a conservative 'fn-decl')."""
    depth = 0
    j = close_idx
    while j >= 0:
        c = js[j]
        if c == ')':
            depth += 1
        elif c == '(':
            depth -= 1
            if depth == 0:
                return j
        j -= 1
    return -1


def _word_before(js, i):
    """(word, start_index) of the identifier ending right before index i (whitespace skipped)."""
    j = _prev_nonspace(js, i)
    if j < 0 or not (js[j].isalnum() or js[j] in '_$'):
        return '', j
    k = j
    while k >= 0 and (js[k].isalnum() or js[k] in '_$'):
        k -= 1
    return js[k + 1:j + 1], k + 1


def _classify_brace(js, i):
    """Classify the '{' at js[i]: ('fn-decl' | 'fn-expr' | 'block', executed_now_or_None).
    fn-expr executed-ness is decided at its closing brace (IIFE) unless it is a load-listener body."""
    p = _prev_nonspace(js, i)
    if p >= 1 and js[p - 1:p + 1] == '=>':
        arrow_start = p - 1
        if LISTENER_ARROW_RE.search(js[max(0, arrow_start - 200):i]):
            return 'fn-expr', True
        return 'fn-expr', None
    if p >= 0 and js[p] == ')':
        o = _matching_open_paren(js, p)
        if o < 0:
            return 'fn-decl', False
        word, wstart = _word_before(js, o)
        if word == 'function':
            if LISTENER_FUNC_RE.search(js[max(0, wstart - 200):i]):
                return 'fn-expr', True
            return 'fn-expr', None          # anonymous function expression
        if word in BLOCK_WORDS:
            return 'block', False           # if/for/while/switch/catch/with: may never run
        if word:
            before, _ = _word_before(js, wstart)
            if before == 'function':
                return 'fn-decl', False     # function name(...) {  (declaration or named expression)
            return 'fn-decl', False         # method shorthand / unknown call-like construct: not executed
        return 'block', True
    word, _ = _word_before(js, i)
    if word == 'else':
        return 'block', False               # the other conditional branch
    return 'block', True                    # object literal, try/finally/do, bare block: runs with its parent


def _iife_starts_statement(js, open_i):
    """True if the function expression whose body opens at js[open_i] == '{' is the start of a
    statement: `(async () => {`, `(function () {` preceded by `;`, `{`, `}` or the script start.
    `false && (() => { ... })()`, `x ? (() => {})() : 0` and `return (() => {})()` are not."""
    p = _prev_nonspace(js, open_i)
    if p >= 1 and js[p - 1:p + 1] == '=>':
        q = _prev_nonspace(js, p - 1)                       # end of the parameter list
        if q >= 0 and js[q] == ')':
            o = _matching_open_paren(js, q)
            if o < 0:
                return False
            q = _prev_nonspace(js, o)
        else:
            word, wstart = _word_before(js, q + 1)          # single identifier parameter
            if not word:
                return False
            q = _prev_nonspace(js, wstart)
    elif p >= 0 and js[p] == ')':
        o = _matching_open_paren(js, p)
        if o < 0:
            return False
        word, wstart = _word_before(js, o)                  # `function` or the expression's name
        if word != 'function':
            before, bstart = _word_before(js, wstart)
            if before != 'function':
                return False
            wstart = bstart
        q = _prev_nonspace(js, wstart)
    else:
        return False
    word, wstart = _word_before(js, q + 1)
    if word == 'async':
        q = _prev_nonspace(js, wstart)
    if q < 0 or js[q] != '(':
        return False                                         # not the `(` that wraps the IIFE
    b = _prev_nonspace(js, q)
    return b < 0 or js[b] in ';{}'


def _iife_after(js, i):
    """True if the '}' at js[i] is immediately invoked: `})(`, `}(`, `})()`."""
    j = i + 1
    while j < len(js) and js[j] in ' \t\r\n':
        j += 1
    if j < len(js) and js[j] == '(':
        return True
    if j < len(js) and js[j] == ')':
        j += 1
        while j < len(js) and js[j] in ' \t\r\n':
            j += 1
        return j < len(js) and js[j] == '('
    return False


def scan_script(js):
    """Tokenise one script block.

    Returns (top_functions, exports, final_depth):
      top_functions: names declared with `function name(` at brace depth 0;
      exports: names assigned via `window.name =` where every enclosing function body executes on
               load (IIFE or load/DOMContentLoaded listener) and no expression-bodied arrow
               encloses the assignment; block braces are transparent."""
    top_functions = set()
    declared_functions = {}     # name -> [enclosing brace ids] of each `function name(` declaration
    assignments = []            # (name, enclosing brace ids, conditional_or_not_callable, rhs identifier or '')
    executed = {}               # brace id -> bool, resolved at close
    stack = []                  # open braces: [id, kind, executed_or_None]
    tpl = []                    # template-literal nesting: 'tpl' or the depth at which a ${ opened
    expr_arrows = []            # expression-bodied arrows: (paren_depth_at_start, brace_depth_at_start)
    i, n, depth, paren, next_id = 0, len(js), 0, 0, 0
    top_unreachable = False     # a top-level `return`/`throw` makes the rest of the script unreachable
    last_kind, last_word, last_char = 'start', '', ''
    while i < n:
        c = js[i]
        if tpl and tpl[-1] == 'tpl':
            if c == '\\':
                i += 2
                continue
            if c == '`':
                tpl.pop()
                i += 1
                last_kind, last_char = 'close', '`'
                continue
            if js.startswith('${', i):
                tpl.append(depth)
                i += 2
                last_kind, last_char = 'punct', '{'
                continue
            i += 1
            continue
        if c in ' \t\r\n':
            i += 1
            continue
        if js.startswith('//', i):
            j = js.find('\n', i)
            i = n if j < 0 else j + 1
            continue
        if js.startswith('/*', i):
            j = js.find('*/', i + 2)
            i = n if j < 0 else j + 2
            continue
        if c in '"\'':
            q = c
            i += 1
            while i < n and js[i] != q:
                i += 2 if js[i] == '\\' else 1
            i += 1
            last_kind, last_char = 'close', q
            continue
        if c == '`':
            tpl.append('tpl')
            i += 1
            continue
        if c == '/':
            regex = (last_kind == 'punct' and last_char in REGEX_AFTER_PUNCT) or last_kind == 'start' \
                or (last_kind == 'word' and last_word in REGEX_AFTER_WORDS)
            if regex:
                i += 1
                in_class = False
                while i < n:
                    ch = js[i]
                    if ch == '\\':
                        i += 2
                        continue
                    if ch == '[':
                        in_class = True
                    elif ch == ']':
                        in_class = False
                    elif ch == '/' and not in_class:
                        break
                    elif ch == '\n':
                        break
                    i += 1
                i += 1
                while i < n and js[i].isalpha():   # flags
                    i += 1
                last_kind, last_char = 'close', '/'
                continue
            last_kind, last_char = 'punct', '/'
            i += 1
            continue
        if IDENT_START.match(c):
            m = IDENT_RE.match(js, i)
            word = m.group(0)
            i = m.end()
            if word == 'async':
                continue                    # transparent: `async function f(){}` is still a declaration
            if word in ('return', 'throw'):
                # an unconditional exit: nothing after it in this block runs (`if (x) return;` is
                # not a statement start and does not count)
                if last_kind == 'start' or (last_kind in ('punct', 'close') and last_char in ';{}'):
                    if stack:
                        stack[-1][4] = True
                    else:
                        top_unreachable = True
            if word == 'function':
                j = i
                while j < n and js[j] in ' \t\r\n*':
                    j += 1
                nm = IDENT_RE.match(js, j)
                # a declaration starts a statement; `x = function f(){}`, `(function f(){})`,
                # `foo(function f(){})` and `return function f(){}` are expressions whose name
                # is scoped to their own body
                statement_start = last_kind == 'start' or (last_kind in ('punct', 'close') and last_char in ';}{')
                if nm and statement_start:
                    declared_functions.setdefault(nm.group(0), []).append(tuple(e[0] for e in stack))
                    if depth == 0:
                        top_functions.add(nm.group(0))
            elif word == 'window':
                am = re.match(r'\s*\.\s*([A-Za-z_$][\w$]*)\s*=(?![=>])', js[i:i + 120])
                if am:
                    # only an assignment that starts a statement is unconditional: `if (x) window.f = f;`,
                    # `else window.f = f;`, `x && (window.f = f)` and `return window.f = f` all follow a
                    # token that is not a statement boundary
                    stmt_start = last_kind == 'start' or (last_kind in ('punct', 'close') and last_char in ';{}')
                    rhs = _rhs_kind(js, i + am.end())
                    unreachable = stack[-1][4] if stack else top_unreachable
                    assignments.append((am.group(1), tuple(e[0] for e in stack),
                                        bool(expr_arrows) or not stmt_start or rhs is None or unreachable, rhs))
            last_kind, last_word, last_char = 'word', word, word[-1]
            continue
        if c.isdigit():
            j = i
            while j < n and (js[j].isalnum() or js[j] in '._'):
                j += 1
            i = j
            last_kind, last_char = 'close', '0'
            continue
        if js.startswith('=>', i):
            # arrow function: a '{' body is classified by _classify_brace; an expression body is
            # tracked until the enclosing call/statement ends
            j = i + 2
            while j < n and js[j] in ' \t\r\n':
                j += 1
            if j >= n or js[j] != '{':
                expr_arrows.append((paren, depth))
            i += 2
            last_kind, last_char = 'punct', '>'
            continue
        if c == '(':
            paren += 1
            last_kind, last_char = 'punct', '('
            i += 1
            continue
        if c == ')':
            paren -= 1
            while expr_arrows and expr_arrows[-1][0] > paren:
                expr_arrows.pop()       # the call that received the arrow closed
            last_kind, last_char = 'close', ')'
            i += 1
            continue
        if c in ',;':
            while expr_arrows and expr_arrows[-1][0] == paren:
                expr_arrows.pop()       # next argument / next statement
            last_kind, last_char = 'punct', c
            i += 1
            continue
        if c == '{':
            kind, ex = _classify_brace(js, i)
            stack.append([next_id, kind, ex, i, False])     # [id, kind, executed, open index, unreachable]
            next_id += 1
            depth += 1
            last_kind, last_char = 'punct', '{'
            i += 1
            continue
        if c == '}':
            if tpl and isinstance(tpl[-1], int) and tpl[-1] == depth:
                tpl.pop()               # closing a ${ } inside a template literal
                i += 1
                continue
            depth -= 1
            while expr_arrows and expr_arrows[-1][1] > depth:
                expr_arrows.pop()       # safety: the block the arrow started in closed
            if stack:
                bid, kind, ex, open_i, _unreachable = stack.pop()
                if ex is None:
                    # an IIFE runs on load only when it is a statement of its own, not an operand
                    ex = kind == 'fn-expr' and _iife_after(js, i) and _iife_starts_statement(js, open_i)
                executed[bid] = bool(ex)
            last_kind, last_char = 'close', '}'
            i += 1
            continue
        last_kind, last_char = ('close' if c == ']' else 'punct'), c
        i += 1
    def visible(rhs, ids):
        # `window.x = f` needs a declaration of f in the assignment's own scope or an enclosing one
        return rhs == '' or any(ids[:len(scope)] == scope for scope in declared_functions.get(rhs, ()))
    exports = {name for name, ids, rejected, rhs in assignments
               if not rejected and all(executed.get(b, False) for b in ids) and visible(rhs, ids)}
    return top_functions, exports, depth


def _rhs_kind(js, i):
    """What is assigned in `window.name = <rhs>` starting at js[i]: '' for a function expression
    (`function`, `async function`, an arrow), the identifier for `window.x = x`, None otherwise
    (`undefined`, `null`, a literal, a call result ...): only the first two can be handlers."""
    m = re.match(r'\s*(async\s+)?function\b', js[i:i + 60])
    if m:
        return ''
    m = re.match(r'\s*(async\s*)?\(', js[i:i + 60])
    if m:
        o = i + m.end() - 1
        depth, j = 0, o
        while j < len(js):
            if js[j] == '(':
                depth += 1
            elif js[j] == ')':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        return '' if re.match(r'\s*=>', js[j + 1:j + 20]) else None
    m = re.match(r'\s*(async\s+)?([A-Za-z_$][\w$]*)\s*(=>)?', js[i:i + 80])
    if m and m.group(2) not in ('undefined', 'null', 'true', 'false', 'new', 'void', 'typeof'):
        return '' if m.group(3) else m.group(2)
    return None


def check_page(name, page, node):
    failures = []
    scripts = SCRIPT_RE.findall(page)
    exposed = set()
    for k, js in enumerate(scripts):
        if node:
            with tempfile.NamedTemporaryFile('w', suffix='.js', delete=False, encoding='utf-8') as f:
                f.write(js)
                tmp = f.name
            try:
                r = subprocess.run([node, '--check', tmp], capture_output=True, text=True)
                if r.returncode != 0:
                    msg = (r.stderr or r.stdout).strip().splitlines()
                    failures.append(f'{name}: script[{k}] fails node --check: ' + ' | '.join(msg[-3:]))
            finally:
                os.unlink(tmp)
        top, exports, final_depth = scan_script(js)
        if final_depth != 0:
            failures.append(f'{name}: script[{k}] brace scan ended at depth {final_depth} (scanner or syntax problem)')
        exposed |= top | exports
    handlers = handler_calls(page)
    for h in sorted(handlers):
        if h in IGNORE_NAMES or h in exposed:
            continue
        failures.append(f'{name}: inline handler "{h}(" is not reachable from global scope '
                        f'(declare it at top level, or assign `window.{h} = {h};` in the page IIFE)')
    return failures, len(scripts), len(handlers)


def selftest(node):
    """Synthetic pages covering the scanner's promises. Returns a list of failed case names."""
    cases = [
        # (case name, page text, expected failing handler names)
        ('regex after return keeps depth', '<script>(async()=>{function f(){ return /}/; } window.f=f;})();</script><b onclick="f()">', set()),
        ('regex after punctuation', '<script>(()=>{const r=[/}/, /\\//g]; function g(){} window.g=g;})();</script><b onclick="g()">', set()),
        ('division is not a regex', '<script>(()=>{const a=4, b=2, c=a/b/1; function h(){} window.h=h;})();</script><b onclick="h()">', set()),
        ('generic event attribute', '<script>(async()=>{function missing(){}})();</script><b ondblclick="missing()">', {'missing'}),
        ('spaced equals', '<script>(async()=>{function m2(){}})();</script><b onclick = "m2()">', {'m2'}),
        ('mixed-case attribute name', '<script>(async()=>{function m7(){}})();</script><b onClick="m7()">', {'m7'}),
        ('unquoted attribute value', '<script>(async()=>{function m8(){}})();</script><b onclick=m8()>', {'m8'}),
        ('escaped attribute in JS string', '<script>(async()=>{function m3(){} const s="<b onclick=\\"m3()\\">";})();</script>', {'m3'}),
        ('every call in a compound attribute', '<script>(async()=>{function ok(){} window.ok=ok; function missing4(){}})();</script><b onclick="ok(); missing4()">', {'missing4'}),
        ('call behind an if guard', '<script>(async()=>{function m5(){}})();</script><b onclick="if (x) m5()">', {'m5'}),
        ('method calls are not handlers', '<script>(async()=>{function n(){} window.n=n;})();</script><b onclick="n(); event.stopPropagation(); a.b.c()">', set()),
        ('template splices run at render time, the outer call at click time', '<script>(async()=>{function esc(s){return s;} function m6(){} const h=`<b onclick="m6(\'${esc(x)}\',${Number(y)||1})">`;})();</script>', {'m6'}),
        ('export inside IIFE', '<script>(async()=>{function a(){} window.a=a;})();</script><b onclick="a()">', set()),
        ('export inside classic IIFE', '<script>(function(){ function d(){} window.d=d; }());</script><b onclick="d()">', set()),
        ('export inside nested IIFE', '<script>(async()=>{(async()=>{function e(){} window.e=e;})();})();</script><b onclick="e()">', set()),
        ('export inside DOMContentLoaded', '<script>document.addEventListener("DOMContentLoaded", async () => { function c(){} window.c=c; });</script><b onclick="c()">', set()),
        ('export inside load function', '<script>window.addEventListener("load", function(){ function c2(){} window.c2=c2; });</script><b onclick="c2()">', set()),
        ('export in uncalled function is not an export', '<script>(async()=>{function never(){ window.b=b; } function b(){}})();</script><b onclick="b()">', {'b'}),
        ('export in block callback is not an export', '<script>(async()=>{Promise.resolve().then(()=>{ window.p=p; }); function p(){}})();</script><b onclick="p()">', {'p'}),
        ('export in expression arrow is not an export', '<script>(async()=>{Promise.resolve().then(() => window.p2 = p2); function p2(){}})();</script><b onclick="p2()">', {'p2'}),
        ('expression arrow ends at the call', '<script>(async()=>{[1].forEach(x => x + 1); function q2(){} window.q2=q2;})();</script><b onclick="q2()">', set()),
        ('expression arrow ends at the statement', '<script>(async()=>{const f2 = x => x * 2; function q3(){} window.q3=q3;})();</script><b onclick="q3()">', set()),
        ('top-level declaration is global', '<script>function g2(){}</script><b onclick="g2()">', set()),
        ('top-level async declaration is global', '<script>async function g3(){}</script><b onclick="g3()">', set()),
        ('named function expression is not global', '<script>const holder = function dead(){}; holder();</script><b onclick="dead()">', {'dead'}),
        ('export in a conditional block is not trusted', '<script>(async()=>{function d2(){} if (false) { window.d2 = d2; }})();</script><b onclick="d2()">', {'d2'}),
        ('export in an else branch is not trusted', '<script>(async()=>{function d3(){} if (true) {} else { window.d3 = d3; }})();</script><b onclick="d3()">', {'d3'}),
        ('export in a try block counts', '<script>(async()=>{function t1(){} try { window.t1 = t1; } catch (e) {}})();</script><b onclick="t1()">', set()),
        ('unbraced if is not trusted', '<script>(async()=>{function u1(){} if (false) window.u1 = u1;})();</script><b onclick="u1()">', {'u1'}),
        ('unbraced else is not trusted', '<script>(async()=>{function u2(){} if (true) {} else window.u2 = u2;})();</script><b onclick="u2()">', {'u2'}),
        ('short-circuit assignment is not trusted', '<script>(async()=>{function u3(){} const x = false; x && (window.u3 = u3);})();</script><b onclick="u3()">', {'u3'}),
        ('ternary assignment is not trusted', '<script>(async()=>{function u4(){} const y = false; y ? window.u4 = u4 : 0;})();</script><b onclick="u4()">', {'u4'}),
        ('assignment after a braced if counts', '<script>(async()=>{function u5(){} if (false) {} window.u5 = u5;})();</script><b onclick="u5()">', set()),
        ('conditionally invoked IIFE is not trusted', '<script>(async()=>{function v1(){} false && (()=>{ window.v1 = v1; })();})();</script><b onclick="v1()">', {'v1'}),
        ('ternary-invoked IIFE is not trusted', '<script>(async()=>{function v2(){} const z = false; z ? (function(){ window.v2 = v2; })() : 0;})();</script><b onclick="v2()">', {'v2'}),
        ('IIFE statement after another statement counts', '<script>(async()=>{function v3(){} const k = 1; (async () => { window.v3 = v3; })();})();</script><b onclick="v3()">', set()),
        ('async IIFE with a named parameter counts', '<script>(async()=>{function v4(){} (async x => { window.v4 = v4; })(1);})();</script><b onclick="v4()">', set()),
        ('export of undefined is not callable', '<script>(async()=>{window.dead = undefined;})();</script><b onclick="dead()">', {'dead'}),
        ('export of a non-function variable is not callable', '<script>(async()=>{const someVar = 1; window.bad2 = someVar;})();</script><b onclick="bad2()">', {'bad2'}),
        ('export of a function expression counts', '<script>(async()=>{window.ok2 = function(){}; window.ok3 = async function(){};})();</script><b onclick="ok2()"><i onclick="ok3()">', set()),
        ('export of an arrow counts', '<script>(async()=>{window.ok4 = () => {}; window.ok5 = x => x; window.ok6 = async (a, b) => a;})();</script><b onclick="ok4()"><i onclick="ok5()"><u onclick="ok6()">', set()),
        ('export of a declared function counts', '<script>(async()=>{window.ok7 = ok7; function ok7(){}})();</script><b onclick="ok7()">', set()),
        ('export of a function declared in another scope is not visible', '<script>function outer(){ function f9(){} } (async()=>{ window.f9 = f9; })();</script><b onclick="f9()">', {'f9'}),
        ('export of a function declared in an enclosing scope counts', '<script>(async()=>{function f10(){} (async()=>{ window.f10 = f10; })();})();</script><b onclick="f10()">', set()),
        ('export after an unconditional return is unreachable', '<script>(async()=>{ function dead3(){} return; window.dead3 = dead3; })();</script><b onclick="dead3()">', {'dead3'}),
        ('export after a guarded return counts', '<script>(async()=>{ function ok8(){} if (false) return; window.ok8 = ok8; })();</script><b onclick="ok8()">', set()),
        ('export after a return inside a block counts', '<script>(async()=>{ function ok9(){} if (false) { return; } window.ok9 = ok9; })();</script><b onclick="ok9()">', set()),
        ('brace inside a splice string does not swallow the handler', '<script>(async()=>{function esc2(s){return s;} function m9(){} const h=`<b onclick="m9(\'${esc2(\'{\')}\')">`;})();</script>', {'m9'}),
        ('brace inside a splice regex does not swallow the handler', '<script>(async()=>{function m10(){} const x="a"; const h=`<b onclick="${/{/.test(x)}; m10()">`;})();</script>', {'m10'}),
        ('template literal braces', '<script>(()=>{const t=`x${ {a:1}.a }y`; function q(){} window.q=q;})();</script><b onclick="q()">', set()),
        ('reserved word in attribute ignored', '<script>(()=>{function z(){} window.z=z;})();</script><b onclick="if(1)z()">', set()),
        ('non-callable browser global is reported', '<script>(()=>{})();</script><b onclick="location()">', {'location'}),
        ('a call inside a string in the attribute is not a handler', '<script>(()=>{})();</script><b onclick="alert(\'missing()\')">', set()),
        ('a call inside a comment in the attribute is not a handler', '<script>(()=>{})();</script><b onclick="alert(1) /* missing2() */">', set()),
        ('a call after a string in the attribute is still checked', '<script>(()=>{})();</script><b onclick="alert(\'x\'); missing3()">', {'missing3'}),
        ('upper-case attribute name on a script-less page', '<b ONCLICK="missing4()">', {'missing4'}),
        ('callable browser global is accepted', '<script>(()=>{})();</script><b onclick="alert(1); open(\'/\')">', set()),
        ('window.x = inside a string ignored', '<script>(async()=>{const str="window.s=s;"; function s(){}})();</script><b onclick="s()">', {'s'}),
    ]
    failed = []
    for cname, page, expect in cases:
        fails, _, _ = check_page('selftest', page, node)
        got = {re.search(r'"(\w+)\(', f).group(1) for f in fails if 'inline handler' in f}
        other = [f for f in fails if 'inline handler' not in f]
        if got != expect or other:
            failed.append(f'{cname}: expected {sorted(expect)}, got {sorted(got)}' + (f'; other: {other}' if other else ''))
    return failed


def main():
    quiet = '--quiet' in sys.argv
    node = shutil.which('node')
    if '--selftest' in sys.argv:
        if not node and '--allow-no-node' not in sys.argv:
            print('node is not on PATH, so the self-test cannot exercise node --check (pass --allow-no-node to skip)')
            return 1
        failed = selftest(node)
        print('\n'.join('  ' + f for f in failed))
        print(f'selftest: {len(failed)} failure(s)' + ('' if node else ' (node not on PATH: syntax checks skipped)'))
        return 1 if failed else 0
    all_fail = []
    if not node:
        if '--allow-no-node' in sys.argv:
            print('WARNING: node not found on PATH; skipping syntax checks', file=sys.stderr)
        else:
            all_fail.append('node is not on PATH, so script syntax cannot be checked (pass --allow-no-node to skip)')
    pages_checked = 0
    for sk in SKETCHES:
        if not os.path.exists(sk):
            all_fail.append(f'sketch not found: {sk}')
            continue
        pages = extract_pages(sk)
        if not pages:
            all_fail.append(f'{os.path.basename(sk)}: no PROGMEM R"HTML( pages found (declaration format changed?)')
            continue
        for name, page in pages.items():
            if '<script' not in page and not ATTR_RE.search(page):
                continue
            fails, ns, nh = check_page(name, page, node)
            pages_checked += 1
            if not quiet:
                print(f'{"FAIL" if fails else "ok  "} {name:<40} scripts={ns} handlers={nh}')
            all_fail += fails
    if pages_checked == 0:
        all_fail.append('no pages were checked')
    for f in all_fail:
        print('  ' + f)
    print(f'{pages_checked} pages checked, {len(all_fail)} problem(s)')
    return 1 if all_fail else 0


if __name__ == '__main__':
    sys.exit(main())
