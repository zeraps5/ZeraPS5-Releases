# Launcher localization

`[interface] idioma` selects one of 15 launcher locales independently of the
console language in `[geral] idioma`. Arabic and Persian use RTL layouts.

The manually maintained base is `translations.tsv`; complete technical UI
messages are in `translations_extra.json`. `technical_translations.json`
contains diagnostic templates, installer messages and help text. Generated
tables are checked in, so building and running requires no translation engine,
network service, Python runtime, language API key or downloaded model.
Technical translations were assisted by local software and partially reviewed;
native speakers should review terminology and grammar in every locale.

Run these offline checks after editing catalogs:

```text
python tools/generate_translations.py
python tools/generate_technical_translations.py
python tools/check_localization.py
ctest --test-dir build --output-on-failure
python tools/test_distribution.py --build build --work build/update-tests
```

`localization_data.json` explicitly lists nontranslatable launcher literals:
OS class/registry identifiers, protocol values, filenames, format-only
compositions and native language names. Do not add human-facing text to this
allowlist to bypass coverage checks. The checks also preserve format arguments,
file identifiers, flags, quoted command names and generated file reproducibility.

## Diagnostics

The original logger and its raw output remain authoritative. When the launcher
starts a child, its private environment block opts into a `.log.records`
sidecar. Normal CLI runs do not enable this extra output. Each binary record
stores a source format and already formatted argument values. It does not
serialize `va_list` or infer C argument types. One CRT-serialized write emits a
record; per-thread bounded scratch avoids large stack allocations in faults.

The launcher renders these records on demand using the current locale, so a
session can be viewed in another language after it ends. The renderer inserts
values without executing the translated text as a printf format. Only fields
explicitly supplied by `ps5c_err_str()` are eligible for error-value localization;
names and other data that happen to equal an error description stay unchanged.
Raw diagnostic parsers, including the missing-system-library check, keep reading
the original log. Third-party raw stderr remains in that original file.

Limits: 16 KiB per structured record, 64 value fields, 64 MiB per session's
optional sidecar, and the last 1 MiB in the text viewer. Unsupported or oversized
records cannot corrupt the following record. Incomplete files get a localized
notice. Old sessions without a sidecar need another run for a translated view.
Reports translate labels and eligible recorded messages before applying the
existing privacy filter; user-written descriptions remain unchanged.

## Help and updates

The technical-help menu opens `docs/HELP-<locale>.txt`. Commands, filenames and
authoritative licenses/credits retain their original form. The update package
includes localized release notes. The installer embeds its own catalog so
recovery does not depend on another language file surviving an interruption.
It reads only the saved interface preference and never rewrites game settings.
Tests exercise all 15 result languages, localized failure/rollback, and native
manifest-note selection against a local HTTP fixture.
