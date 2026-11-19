#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reject uncatalogued launcher text and check generated artifacts, offline."""
from pathlib import Path
import ast,hashlib,json,re,subprocess,sys
root=Path(__file__).resolve().parents[1]
known=set()
for row in (root/'frontends/launcher/translations.tsv').read_text(encoding='utf8').splitlines():
    if row and not row.startswith('#'):
        known.add(row.split('|')[0].replace('\\r','\r').replace('\\n','\n'))
known.update(json.loads((root/'frontends/launcher/translations_extra.json').read_text(encoding='utf8')))
data=set(json.loads((root/'frontends/launcher/localization_data.json').read_text(encoding='utf8')))
source=(root/'frontends/launcher/launcher.c').read_text(encoding='utf8')
for match in re.finditer(r'L"(?:[^"\\]|\\.)*"(?:\s*L"(?:[^"\\]|\\.)*")*',source):
    text=''.join(ast.literal_eval(v[1:])for v in re.findall(r'L"(?:[^"\\]|\\.)*"',match[0]))
    assert not text or text in known or text in data,(source[:match.start()].count('\n')+1,text)
generated=[root/'frontends/launcher/translations.inc',root/'frontends/launcher/log_translations.inc',root/'tools/apply-update.ps1',*root.glob('dist/docs/HELP-*.txt')]
digest=lambda file:hashlib.sha256(file.read_bytes()).hexdigest()
before={file:digest(file)for file in generated}
for tool in ['generate_translations.py','generate_technical_translations.py']:
    subprocess.run([sys.executable,str(root/'tools'/tool)],check=True)
assert all(digest(file)==value for file,value in before.items()),'Generated files are stale'
assert 'TEMPORARY-CATALOG' not in (root/'frontends/launcher/log_translations.inc').read_text(encoding='utf8')
catalog=json.loads((root/'frontends/launcher/technical_translations.json').read_text(encoding='utf8'))
entries=catalog+[{'source':key,'text':value}for key,value in json.loads((root/'frontends/launcher/translations_extra.json').read_text(encoding='utf8')).items()]
identifiers=re.compile(r'\b(?:PS5C_[A-Z0-9_]+|VK_[A-Z0-9_]+|[A-Za-z0-9_.-]+\.(?:exe|ps1|ini|json|sprx|txt|zip|bin|exfat))\b')
for entry in entries:
    for tag,text in entry['text'].items():
        assert '\u2047'not in text,(entry['source'],tag,'unresolved translation token')
        for token in identifiers.findall(entry['source']):assert token in text,(token,tag,entry['source'])
        for token in re.findall(r"['`]([A-Za-z][A-Za-z0-9_-]*)['`]",entry['source']):
            assert re.search(r'(?<![A-Za-z0-9_])'+re.escape(token)+r'(?![A-Za-z0-9_])',text),(token,tag,entry['source'])
updates={x['source']for x in catalog if x['kind']=='installer'}
script=(root/'tools/apply-update.ps1').read_text(encoding='utf-8-sig')
for key in re.findall(r"Update-Text '([^']+)'",script):assert key in updates,key
assert len(list(root.glob('dist/docs/HELP-*.txt')))==15
print('Launcher text coverage, installer messages and generated catalogs verified in 15 languages')
