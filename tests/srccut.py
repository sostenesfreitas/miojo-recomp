"""Cutting a piece of C, C++ or GLSL source out by its text, for the checks that compile or read one function of a source file
(the dev-loop proposal's D2, #2636). Standard library only.

    sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
    from srccut import block, between

block(text, marker): from the marker to the brace that closes the first { after it. Braces in string and character
literals (raw strings too), comments and preprocessor lines are not counted, as the copies of a brace counter in the checks
counted them. between(text, start, end): from start up to, not including, the first end after it.

A marker that is missing, or that appears more than once, stops the check with one message naming the source and the
marker: a function renamed or reshaped in the source is then one clear failure, and a marker that matches two places (a
declaration and its definition) cannot cut the wrong one. unique=False takes the first of several on purpose.

SRCCUT_CHECK_NAIVE=1 also cuts each block as the old brace counter did and stops if the two differ: what a migration from
a check's own copy runs once to show the check cuts the same text as before.
"""
import os

__all__ = ["block", "between", "find", "CutError"]


class CutError(SystemExit):
    """A marker missing, repeated, or a block that does not close: the check stops with this message"""


def find(text, marker, where="the source", unique=True, start=0):
    """Where marker is in text: the only place (or the first, unique=False)"""
    at = text.find(marker, start)
    if at < 0:
        raise CutError("srccut: %r is not in %s (renamed or reshaped? update the check with the source)" % (marker, where))
    if unique:
        again = text.find(marker, at + 1)
        if again >= 0:
            raise CutError("srccut: %r is in %s more than once (lines %d and %d): make the marker longer, or pass unique=False"
                           % (marker, where, text.count("\n", 0, at) + 1, text.count("\n", 0, again) + 1))
    return at


def _skip_literal(text, i, quote):
    """The index after the string or character literal whose opening quote is at i"""
    i += 1
    while i < len(text):
        c = text[i]
        if c == "\\":
            i += 2
            continue
        if c == quote or c == "\n":   # (an unclosed literal ends at the line's end, as a compiler would complain)
            return i + 1
        i += 1
    return i


def _raw_string_end(text, i):
    """i is at R"delim( : the index after its )delim\" """
    open_paren = text.find("(", i + 2)
    delim = text[i + 2:open_paren]
    close = text.find(")" + delim + '"', open_paren + 1)
    return len(text) if close < 0 else close + len(delim) + 2


def _digit_separator(text, i):
    """The ' at i is C++14's digit separator (1'000'000), not a character literal"""
    j = i - 1
    while j >= 0 and (text[j].isalnum() or text[j] in "'_"):
        j -= 1
    token = text[j + 1:i]
    return bool(token) and token[0].isdigit() and i + 1 < len(text) and text[i + 1].isalnum()


def _closing_brace(text, open_at, where, marker):
    """The index after the } that closes the { at open_at, not counting braces in literals, comments or preprocessor lines"""
    depth, i, n = 0, open_at, len(text)
    line_start = True
    while i < n:
        c = text[i]
        if line_start and c in " \t":
            i += 1
            continue
        if line_start and c == "#":   # (a preprocessor line, with its continuations)
            while i < n and text[i] != "\n":
                i += 2 if text[i] == "\\" else 1
            continue
        line_start = c == "\n"
        if text.startswith("//", i):
            nl = text.find("\n", i)
            i = n if nl < 0 else nl
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            continue
        if c == '"':
            if i and text[i - 1] == "R" and (i < 2 or not (text[i - 2].isalnum() or text[i - 2] == "_") or text[i - 2] in "uUL8"):
                i = _raw_string_end(text, i - 1)
            else:
                i = _skip_literal(text, i, '"')
            continue
        if c == "'" and not _digit_separator(text, i):
            i = _skip_literal(text, i, "'")
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise CutError("srccut: the block at %r in %s does not close" % (marker, where))


def _naive(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def block(text, marker, where="the source", unique=True):
    """From marker to the brace that closes the first { after it"""
    start = find(text, marker, where, unique)
    brace = text.find("{", start)
    if brace < 0:
        raise CutError("srccut: no { after %r in %s" % (marker, where))
    cut = text[start:_closing_brace(text, brace, where, marker)]
    if os.environ.get("SRCCUT_CHECK_NAIVE") == "1" and cut != _naive(text, marker):
        raise CutError("srccut: %r in %s cuts differently from the old brace counter (a brace in a string or a comment?)"
                       % (marker, where))
    return cut


def between(text, start, end, where="the source", unique=True):
    """From start up to, not including, the first end after it"""
    at = find(text, start, where, unique)
    stop = text.find(end, at + len(start))
    if stop < 0:
        raise CutError("srccut: %r does not follow %r in %s" % (end, start, where))
    return text[at:stop]
