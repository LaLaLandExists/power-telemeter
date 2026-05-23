"""Pure-Python JS, CSS, and HTML minifiers -- no external dependencies."""

import re


# ---------------------------------------------------------------------------
# JS  (state machine)
# ---------------------------------------------------------------------------

def jsmin(text):
    """Strip JS comments and collapse whitespace.

    Handles: single/double/template string literals, line comments (//),
    block comments (/* */), and preserved license comments (/*!).

    Known limitation: regex literals whose body contains // or /* are not
    distinguished from division operators -- acceptable for typical dashboard
    code that does not embed comments inside regex literals.
    """
    NORMAL, SQ, DQ, BT, LC, BC = range(6)
    state = NORMAL
    out = []
    i = 0
    n = len(text)
    bt_depth = 0  # brace nesting depth inside ${ } inside a template literal

    while i < n:
        c = text[i]

        if state == NORMAL:
            if c == "'":
                state = SQ
                out.append(c)

            elif c == '"':
                state = DQ
                out.append(c)

            elif c == '`':
                state = BT
                bt_depth = 0
                out.append(c)

            elif c == '/' and i + 1 < n:
                nc = text[i + 1]
                if nc == '/':
                    state = LC
                    i += 2
                    continue
                elif nc == '*':
                    if i + 2 < n and text[i + 2] == '!':
                        # License comment -- find end and preserve verbatim.
                        j = i + 2
                        while j < n - 1:
                            if text[j] == '*' and text[j + 1] == '/':
                                out.append(text[i:j + 2])
                                i = j + 2
                                break
                            j += 1
                        else:
                            i = n
                        continue
                    else:
                        state = BC
                        i += 2
                        continue
                else:
                    out.append(c)

            elif c in (' ', '\t', '\r', '\n'):
                # Consume the entire whitespace run.
                while i + 1 < n and text[i + 1] in (' ', '\t', '\r', '\n'):
                    i += 1
                # Emit one space only when needed to separate identifier tokens.
                prev = out[-1] if out else ''
                nxt  = text[i + 1] if i + 1 < n else ''
                if _is_id(prev) and _is_id(nxt):
                    out.append(' ')

            else:
                out.append(c)

        elif state == SQ:
            out.append(c)
            if c == '\\' and i + 1 < n:
                i += 1
                out.append(text[i])
            elif c == "'":
                state = NORMAL

        elif state == DQ:
            out.append(c)
            if c == '\\' and i + 1 < n:
                i += 1
                out.append(text[i])
            elif c == '"':
                state = NORMAL

        elif state == BT:
            out.append(c)
            if c == '\\' and i + 1 < n:
                i += 1
                out.append(text[i])
            elif c == '$' and i + 1 < n and text[i + 1] == '{':
                # Opening ${ -- track brace depth so we know when the
                # template expression ends and the BT string resumes.
                out.append('{')
                i += 2
                bt_depth += 1
                continue
            elif c == '{' and bt_depth > 0:
                bt_depth += 1
            elif c == '}' and bt_depth > 0:
                bt_depth -= 1
            elif c == '`' and bt_depth == 0:
                state = NORMAL

        elif state == LC:
            if c == '\n':
                state = NORMAL
                if out and _is_id(out[-1]):
                    out.append(' ')

        elif state == BC:
            if c == '*' and i + 1 < n and text[i + 1] == '/':
                state = NORMAL
                i += 2
                if out and _is_id(out[-1]):
                    out.append(' ')
                continue

        i += 1

    return ''.join(out).strip()


def _is_id(c):
    """True when c can be part of an identifier, keyword, or number literal."""
    return c.isalnum() or c in ('_', '$')


# ---------------------------------------------------------------------------
# CSS  (regex-based -- unambiguous enough to be safe)
# ---------------------------------------------------------------------------

def cssmin(text):
    """Strip CSS comments and collapse whitespace."""
    # Remove all /* ... */ comments (non-greedy, multiline).
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)
    # Collapse any whitespace run to a single space.
    text = re.sub(r'[ \t\r\n]+', ' ', text)
    # Remove spaces around structural characters.
    text = re.sub(r'\s*([{}])\s*', r'\1', text)
    text = re.sub(r';\s*', ';', text)
    text = re.sub(r',\s*', ',', text)
    # Remove spaces around colons (property: value -> property:value).
    # Safe because CSS colons in URLs have no surrounding whitespace.
    text = re.sub(r'\s*:\s*', ':', text)
    # Drop trailing semicolons before a closing brace.
    text = re.sub(r';}', '}', text)
    return text.strip()


# ---------------------------------------------------------------------------
# HTML  (regex-based)
# ---------------------------------------------------------------------------

def htmlmin(text):
    """Strip HTML comments and collapse inter-tag whitespace."""
    # Remove HTML comments; preserve IE conditional comments (<!--[if ...]).
    text = re.sub(r'<!--(?!\[if\b).*?-->', '', text, flags=re.DOTALL)
    # Collapse horizontal whitespace to a single space.
    text = re.sub(r'[ \t]+', ' ', text)
    # Strip leading whitespace on every line.
    text = re.sub(r'\n\s+', '\n', text)
    # Collapse consecutive blank lines.
    text = re.sub(r'\n{2,}', '\n', text)
    # Remove whitespace between adjacent tags.
    text = re.sub(r'>\s+<', '><', text)
    return text.strip()
