#!/usr/bin/env python3
"""Build the .ymo translation resources and translate.rc from the .po files.

Requires polib:  pip install -r requirements.txt

The pipeline this replaced needed a patched fork of translate-toolkit that could not be
reproduced from PyPI, so nobody could run it and translate.rc sat empty in the
repository while the application silently ran untranslated. polib is an ordinary
published package, so that failure mode is gone.

Usage:
    python build_translations.py            # build generated/*.ymo and generated/translate.rc
    python build_translations.py --check    # report strings in the source that no .po covers

The .ymo format is unchanged, so I18n.hpp needs no modification:

    uint16  count
    count * { uint32 fnv1a_32(key)  ;  uint16 offset }
    ... NUL-terminated UTF-16LE strings, in table order ...

`key` is the untranslated string as it appears in the source, UTF-16LE encoded and
hashed without its terminator -- matching Translate() in I18n.hpp. For C_() context
lookups the key is `context + "\\x04" + text`, matching the C_ macro.
"""

import argparse
import re
import sys
from pathlib import Path

try:
    import polib
except ImportError:
    # Name the interpreter: a machine with several Python installs will happily run this
    # under one that has no polib while pip put it in another.
    sys.exit(
        f"error: polib is not installed for {sys.executable}\n"
        f'    "{sys.executable}" -m pip install -r translate/requirements.txt'
    )

FNV1_32_INIT = 0x811C9DC5
FNV_32_PRIME = 0x01000193
ENCODING = "utf-16-le"

# Resource languages. Adding a locale requires an explicit entry here rather than a
# guess, because FindResourceExW matches on exactly these values at runtime.
RC_LANGUAGES = {
    "zh_CN": ("LANG_CHINESE", "SUBLANG_CHINESE_SIMPLIFIED"),
    "zh_TW": ("LANG_CHINESE", "SUBLANG_CHINESE_TRADITIONAL"),
}

# Source files scanned by --check.
SCAN_GLOBS = ("*.cpp", "*.hpp")

# Escapes in C++ string literals, for the --check scanner. polib already resolves these
# inside .po files, so this is only used on the source.
CPP_ESCAPES = {
    "n": "\n", "t": "\t", "r": "\r", "a": "\a",
    "b": "\b", "f": "\f", "v": "\v", '"': '"', "\\": "\\",
}


def fnv1a_32(data: bytes) -> int:
    h = FNV1_32_INIT
    for byte in data:
        h ^= byte
        h = (h * FNV_32_PRIME) & 0xFFFFFFFF
    return h


def unescape(text: str) -> str:
    out = []
    i = 0
    while i < len(text):
        c = text[i]
        if c == "\\" and i + 1 < len(text):
            nxt = text[i + 1]
            if nxt in CPP_ESCAPES:
                out.append(CPP_ESCAPES[nxt])
                i += 2
                continue
            if nxt == "x":  # \xNN
                m = re.match(r"x([0-9a-fA-F]{1,4})", text[i + 1:])
                if m:
                    out.append(chr(int(m.group(1), 16)))
                    i += 1 + len(m.group(0))
                    continue
            m = re.match(r"[0-7]{1,3}", text[i + 1:])  # octal
            if m:
                out.append(chr(int(m.group(0), 8)))
                i += 1 + len(m.group(0))
                continue
        out.append(c)
        i += 1
    return "".join(out)


class PoError(Exception):
    pass


def parse_po(path: Path):
    """Yield (key, translation) for every translated, non-fuzzy entry."""
    for key, translation, flags in parse_po_all(path):
        if translation and "fuzzy" not in flags:
            yield key, translation


def parse_po_all(path: Path):
    """Yield (key, translation, flags) for every entry, translated or not.

    polib skips the header and obsolete (#~) entries when iterating, and resolves the
    escape sequences, so the strings arrive as the compiler would have produced them --
    which is what the hash has to be computed over.
    """
    for entry in polib.pofile(str(path)):
        if entry.msgid_plural:
            raise PoError(
                f"{path.name}: plural forms are not supported ({entry.msgid!r}). "
                f"The .ymo format stores one string per key."
            )
        # The C_ macro looks up `context \x04 text`; see I18n.hpp.
        key = f"{entry.msgctxt}\x04{entry.msgid}" if entry.msgctxt else entry.msgid
        yield key, entry.msgstr, set(entry.flags)


def build_ymo(po_path: Path, out_path: Path) -> int:
    units = {}  # hash -> (key, encoded translation)

    for key, translation in parse_po(po_path):
        h = fnv1a_32(key.encode(ENCODING))
        if h in units and units[h][0] != key:
            raise PoError(
                f"{po_path.name}: FNV-1a collision between\n"
                f"    {units[h][0]!r}\n  and\n    {key!r}\n"
                f"  Reword one of them; the runtime looks up by hash alone and "
                f"would silently return the wrong string."
            )
        units[h] = (key, translation.encode(ENCODING) + b"\x00\x00")

    count = len(units)
    if count > 0xFFFF:
        raise PoError(f"{po_path.name}: {count} strings exceeds the uint16 count field")

    table_size = 2 + count * 6
    total = table_size + sum(len(data) for _, data in units.values())
    if total > 0xFFFF:
        raise PoError(
            f"{po_path.name}: .ymo would be {total} bytes; offsets are uint16 so the "
            f"limit is 65535. The format needs widening before more strings fit."
        )

    blob = bytearray()
    blob += count.to_bytes(2, "little")

    offset = table_size
    for h, (_, data) in units.items():
        blob += h.to_bytes(4, "little")
        blob += offset.to_bytes(2, "little")
        offset += len(data)

    for _, data in units.values():
        blob += data

    out_path.write_bytes(blob)
    return count


def write_rc(out_path: Path, locales) -> None:
    lines = [
        "// Generated by build_translations.py -- do not edit.",
        '#include "../../targetver.h"',
        '#include "windows.h"',
        "",
    ]
    for locale in locales:
        lang, sublang = RC_LANGUAGES[locale]
        lines.append(f"LANGUAGE {lang}, {sublang}")
        lines.append(f'1 YMO "{locale}.ymo"')
        lines.append("")
    out_path.write_text("\n".join(lines), encoding="utf-16")


def iter_source_strings(root: Path):
    """Yield (text, file, line) for every _() and C_() literal in the source."""
    plain = re.compile(r'(?<![A-Za-z0-9_])_\(\s*L"((?:[^"\\]|\\.)*)"\s*\)')
    ctxt = re.compile(
        r'(?<![A-Za-z0-9_])C_\(\s*L"((?:[^"\\]|\\.)*)"\s*,\s*L"((?:[^"\\]|\\.)*)"\s*\)'
    )
    for pattern in SCAN_GLOBS:
        for path in sorted(root.glob(pattern)):
            for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                for m in ctxt.finditer(line):
                    yield f"{unescape(m.group(1))}\x04{unescape(m.group(2))}", path.name, lineno
                for m in plain.finditer(line):
                    yield unescape(m.group(1)), path.name, lineno


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="report source strings that no .po translates, and exit")
    args = parser.parse_args()

    here = Path(__file__).resolve().parent
    source_dir = here / "source"
    out_dir = here / "generated"
    repo_root = here.parent

    po_files = sorted(source_dir.glob("*.po"))
    if not po_files:
        print(f"error: no .po files in {source_dir}", file=sys.stderr)
        return 1

    if args.check:
        found = list(iter_source_strings(repo_root))
        if not found:
            print("error: no _() strings found in the source; the scanner is broken",
                  file=sys.stderr)
            return 1
        unique = {text for text, _, _ in found}
        print(f"{len(unique)} distinct translatable string(s) across "
              f"{len(found)} call site(s).\n")
        gaps_total = 0
        for po in po_files:
            entries = list(parse_po_all(po))
            known = {key: translation for key, translation, _ in entries}
            fuzzy = {key for key, _, flags in entries if "fuzzy" in flags}
            translated = {key for key, _ in parse_po(po)}

            # Absent from the catalogue is a gap someone has to fill. Present but
            # deliberately left empty (the app name, for one) is a decision, not a
            # gap, so it must not be reported the same way. A fuzzy entry has a
            # draft msgstr but parse_po drops it, so the runtime has nothing --
            # that is a gap too, and it must fail --check like an absent key.
            absent = [(t, f, n) for t, f, n in found if t not in known]
            empty = sorted({t for t, _, _ in found
                            if t in known and not known[t] and t not in fuzzy})
            fuzzy_used = sorted({t for t, _, _ in found if t in fuzzy})

            print(f"{po.name}: {len(translated & unique)}/{len(unique)} translated")
            if absent:
                print(f"  missing from the catalogue ({len(absent)}):")
                for text, fname, lineno in absent:
                    print(f"    {fname}:{lineno}  {text!r}")
            if fuzzy_used:
                print(f"  fuzzy, not compiled -- needs review ({len(fuzzy_used)}):")
                for text in fuzzy_used:
                    print(f"    {text!r}")
            if empty:
                print(f"  present but intentionally untranslated ({len(empty)}):")
                for text in empty:
                    print(f"    {text!r}")
            gaps_total += len(absent) + len(fuzzy_used)
        return 1 if gaps_total else 0

    out_dir.mkdir(parents=True, exist_ok=True)
    locales = []
    for po in po_files:
        locale = po.stem
        if locale not in RC_LANGUAGES:
            print(f"error: {po.name}: no resource language mapped for {locale!r}. "
                  f"Add it to RC_LANGUAGES.", file=sys.stderr)
            return 1
        ymo = out_dir / f"{locale}.ymo"
        count = build_ymo(po, ymo)
        if count == 0:
            print(f"error: {po.name} produced no translations", file=sys.stderr)
            return 1
        print(f"{po.name} -> {ymo.name}  ({count} strings, {ymo.stat().st_size} bytes)")
        locales.append(locale)

    rc = out_dir / "translate.rc"
    write_rc(rc, locales)
    print(f"-> {rc.name}  ({len(locales)} language(s))")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except PoError as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
