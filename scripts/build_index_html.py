"""PlatformIO pre-build hook: minify src/index_html.src.html into a generated
header that defines INDEX_HTML[]. The generated header lives under the build
directory so the source tree stays clean.

Conservative minification:
  - HTML: strip <!-- ... --> comments; collapse interstitial whitespace
          (between tags) and runs of whitespace inside tags.
  - CSS:  inside <style>, strip /* ... */ comments and collapse whitespace,
          including around { } : ; , ;.
  - JS:   inside <script>, strip // line and /* */ block comments via a
          string-aware state machine. **JS whitespace is preserved verbatim**
          so ASI (automatic semicolon insertion) and template literals are
          unaffected.
"""

import os
import re

Import("env")  # noqa: F821  (provided by SCons)


def strip_js_comments(src: str) -> str:
    """State-machine pass that strips // and /*...*/ comments while respecting
    single/double/backtick strings and regex literals. Whitespace is otherwise
    preserved exactly."""
    out = []
    i, n = 0, len(src)
    # Track previous non-whitespace token to decide whether a `/` starts a regex.
    prev_token = ""

    def is_regex_context(prev: str) -> bool:
        # If the previous non-whitespace token ends in something that can be
        # followed by a regex (operators, keywords), `/` starts a regex literal.
        if not prev:
            return True
        if prev[-1] in "(,=:[!&|?{};+~<>%^*-":
            return True
        kw = ("return", "typeof", "instanceof", "in", "of", "new",
              "delete", "void", "throw", "case", "do", "else", "yield", "await")
        return any(prev.endswith(k) for k in kw)

    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""

        # Line comment
        if c == "/" and nxt == "/":
            j = src.find("\n", i)
            if j == -1:
                break
            i = j  # keep the newline
            continue

        # Block comment
        if c == "/" and nxt == "*":
            j = src.find("*/", i + 2)
            if j == -1:
                break
            i = j + 2
            # Preserve a single space if we removed something between tokens
            out.append(" ")
            continue

        # String literals
        if c in ('"', "'", "`"):
            quote = c
            out.append(c)
            i += 1
            while i < n:
                ch = src[i]
                out.append(ch)
                if ch == "\\" and i + 1 < n:
                    out.append(src[i + 1])
                    i += 2
                    continue
                i += 1
                if ch == quote:
                    break
            prev_token = quote
            continue

        # Regex literal — only if context allows.
        if c == "/" and is_regex_context(prev_token):
            out.append(c)
            i += 1
            in_class = False
            while i < n:
                ch = src[i]
                out.append(ch)
                if ch == "\\" and i + 1 < n:
                    out.append(src[i + 1])
                    i += 2
                    continue
                i += 1
                if ch == "[":
                    in_class = True
                elif ch == "]":
                    in_class = False
                elif ch == "/" and not in_class:
                    # consume flags
                    while i < n and src[i].isalpha():
                        out.append(src[i])
                        i += 1
                    break
            prev_token = "/regex/"
            continue

        out.append(c)
        if not c.isspace():
            prev_token = (prev_token + c)[-16:]
        i += 1

    return "".join(out)


def minify_css(src: str) -> str:
    # Strip /* ... */ comments
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.DOTALL)
    # Collapse whitespace
    src = re.sub(r"\s+", " ", src)
    # Strip whitespace around CSS punctuation
    src = re.sub(r"\s*([{}:;,>+~])\s*", r"\1", src)
    # Drop trailing semicolons before }
    src = re.sub(r";}", "}", src)
    return src.strip()


def minify_html_outer(src: str) -> str:
    # Strip HTML comments (excluding conditional comments — none here)
    src = re.sub(r"<!--.*?-->", "", src, flags=re.DOTALL)
    # Collapse whitespace runs
    src = re.sub(r"\s+", " ", src)
    # Strip whitespace between tags
    src = re.sub(r">\s+<", "><", src)
    return src.strip()


def minify(src: str) -> str:
    """Split on <style>, <script>, <pre>, and <textarea> blocks; minify each
    with the right strategy, reassemble. <pre>/<textarea> bodies are passed
    through verbatim so whitespace-significant content (ASCII art, code
    samples) survives."""
    parts = re.split(
        r"(<style[^>]*>.*?</style>"
        r"|<script[^>]*>.*?</script>"
        r"|<pre[^>]*>.*?</pre>"
        r"|<textarea[^>]*>.*?</textarea>)",
        src, flags=re.DOTALL)
    out = []
    for part in parts:
        if part.startswith("<style"):
            m = re.match(r"(<style[^>]*>)(.*?)(</style>)", part, flags=re.DOTALL)
            out.append(m.group(1) + minify_css(m.group(2)) + m.group(3))
        elif part.startswith("<script"):
            m = re.match(r"(<script[^>]*>)(.*?)(</script>)", part, flags=re.DOTALL)
            js = strip_js_comments(m.group(2))
            # Light whitespace cleanup at line boundaries: collapse runs of
            # blank lines but keep newlines (ASI-safe).
            js = re.sub(r"\n[ \t]+\n", "\n\n", js)
            js = re.sub(r"\n{3,}", "\n\n", js)
            # Trim trailing whitespace on each line.
            js = re.sub(r"[ \t]+\n", "\n", js)
            out.append(m.group(1) + js + m.group(3))
        elif part.startswith("<pre") or part.startswith("<textarea"):
            out.append(part)
        else:
            out.append(minify_html_outer(part))
    return "".join(out)


def build_header(env):
    proj = env["PROJECT_DIR"]
    script_path = os.path.join(proj, "scripts", "build_index_html.py")
    src_path = os.path.join(proj, "src", "index_html.src.html")
    build_dir = env.subst("$BUILD_DIR")
    gen_dir = os.path.join(build_dir, "generated")
    dst_path = os.path.join(gen_dir, "index_html.h")

    if not os.path.exists(src_path):
        # Source missing — leave any existing generated header in place.
        env.Append(CPPPATH=[gen_dir])
        return

    os.makedirs(gen_dir, exist_ok=True)

    if (os.path.exists(dst_path)
            and os.path.getmtime(dst_path) >= os.path.getmtime(src_path)
            and os.path.getmtime(dst_path) >= os.path.getmtime(script_path)):
        env.Append(CPPPATH=[gen_dir])
        return

    with open(src_path, "r", encoding="utf-8") as f:
        raw = f.read()
    minified = minify(raw)

    with open(dst_path, "w", encoding="utf-8") as f:
        f.write("// Auto-generated from src/index_html.src.html — do not edit.\n")
        f.write("// Regenerate via PlatformIO build (scripts/build_index_html.py).\n")
        f.write("#pragma once\n")
        f.write("#include <pgmspace.h>\n")
        f.write('static const char INDEX_HTML[] PROGMEM = R"HTML(')
        f.write(minified)
        f.write(')HTML";\n')

    print("[index_html] {} -> {} bytes ({} src -> {} min)".format(
        os.path.relpath(src_path, proj),
        len(minified),
        len(raw),
        len(minified)))

    env.Append(CPPPATH=[gen_dir])


build_header(env)  # noqa: F821
