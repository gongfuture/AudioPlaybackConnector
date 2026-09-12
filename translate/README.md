# Translations

## The three file types

| File | What it is | Who writes it |
|---|---|---|
| `source/messages.pot` | Template: every translatable string in the source, all untranslated. Used as the diff base, and as the starting point for a new language. | `update_catalogs.ps1` |
| `source/<locale>.po` | One language's catalogue: the strings plus their translations. | A translator, updated by `update_catalogs.ps1` |
| `generated/<locale>.ymo` | Compact lookup table linked into the executable as a `YMO` resource. | `build_translations.py` |

A string becomes translatable by wrapping it in `_()`, or `C_(context, text)` when the
same English word needs different translations in different places. At runtime
`Translate()` in `I18n.hpp` hashes the source string with FNV-1a and looks it up in the
`.ymo` for the current UI language, falling back to the English text when there is no
entry — so a missing translation degrades to English rather than breaking.

## Building the resources — needs Python + polib

`build_translations.py` turns the `.po` files into `.ymo` plus `generated/translate.rc`,
and **the project build runs it automatically** whenever a `.po` changes.

```
pip install -r translate\requirements.txt           # once

python translate\build_translations.py              # rebuild the .ymo resources
python translate\build_translations.py --check      # list strings no .po covers
```

The generated files are committed and the build step only warns if it cannot run, so a
plain clone still builds with translations without Python or polib installed. You only
need them to change a translation.

`--check` exits non-zero when a string in the source is missing from a catalogue, so it
works as a CI gate. It reports "missing from the catalogue" separately from "present but
intentionally untranslated" — the application name is deliberately left untranslated and
is not a gap.

## Updating the catalogues — needs GNU gettext

After adding, removing, or rewording any `_()` string, re-extract and merge:

```
pwsh translate\update_catalogs.ps1
```

This runs `xgettext` to regenerate `messages.pot` with correct `file:line` references,
then `msgmerge` to fold it into every `.po`: existing translations are kept, new strings
appear untranslated, deleted ones are marked obsolete (`#~`), and reworded ones carry the
old text over flagged `fuzzy` so a translator confirms rather than silently loses it.

Install gettext with either:

```
choco install gettext
winget install --id mlocati.GettextIconv
```

The script also looks in the default install directory, so it works without reopening the
terminal after installing. **gettext is only needed to update the catalogues — building
the project does not require it.**

The `#:` line references are informational, to help a translator find a string in the
source. Stale ones break nothing at runtime.

## Adding a language

1. Copy `source/messages.pot` to `source/<locale>.po` and fill in the header.
2. Add the locale to `RC_LANGUAGES` in `build_translations.py`, mapping it to the
   `LANG_*` / `SUBLANG_*` pair that `FindResourceExW` will match at runtime. The script
   fails rather than guessing, because a wrong pair means the resource is never found.
3. Translate, then build.

## Notes

- The `.ymo` format uses 16-bit offsets, so a catalogue is capped at 64 KB, and lookups
  are by 32-bit hash alone. `build_translations.py` fails loudly on a hash collision or
  an oversized catalogue rather than silently producing wrong strings.
- This pipeline previously depended on a patched fork of translate-toolkit that could not
  be reproduced from PyPI, and `generated/` was git-ignored while holding a tracked empty
  `translate.rc`. The result was that every clone silently ran untranslated. Both are
  fixed; keep the generated files committed and the build step in place, and keep any
  dependency to something `pip install` can actually fetch.
