"""Check public documentation against GitHub's actual latest release."""
from pathlib import Path
import argparse
import json
import os
import re
import urllib.parse
import urllib.request


def check(root):
    headers = {'User-Agent': 'ZeraPS5-release-docs-check', 'Accept': 'application/vnd.github+json'}
    token = os.environ.get('GITHUB_TOKEN')
    if token:
        headers['Authorization'] = 'Bearer ' + token
    request = urllib.request.Request('https://api.github.com/repos/zeraps5/ZeraPS5-Releases/releases/latest', headers=headers)
    with urllib.request.urlopen(request, timeout=30) as response:
        release = json.load(response)
    tag = release['tag_name']
    assert re.fullmatch(r'v\d+\.\d+\.\d+-b\d+', tag), 'Unexpected release tag'
    assert not release['draft'] and not release['prerelease'], 'Latest release must be published'
    assets = {a['name']: a for a in release['assets']}
    windows = f'ZeraPS5-{tag}-win64.zip'
    source = f'ZeraPS5-{tag}-win64-source.zip'
    required = [windows, source, 'latest.json', 'SHA256SUMS', windows + '.sha256', windows.replace('.zip', '.manifest.json')]
    assert all(name in assets and assets[name]['size'] > 0 for name in required), 'Missing release assets'
    readme = (root / 'README.md').read_text(encoding='utf-8')
    compat = (root / 'COMPATIBILITY.md').read_text(encoding='utf-8')
    provenance = (root / 'SOURCE-VERSION.txt').read_text(encoding='utf-8')
    changelog = (root / 'CHANGELOG.md').read_text(encoding='utf-8')
    assert f'**Current release: {tag}.**' in readme, 'README current release is stale'
    for name in [windows, source]:
        assert assets[name]['browser_download_url'] in readme, 'README download link is stale'
    assert f'Public release: {tag}\n' in provenance, 'Source provenance is stale'
    for name, label in [(windows, 'Windows archive SHA-256'), (source, 'Source archive SHA-256')]:
        digest = assets[name].get('digest', '')
        assert re.fullmatch('sha256:[0-9a-f]{64}', digest), 'Missing GitHub asset digest'
        assert f'{label}: {digest[7:]}' in provenance, 'Source provenance checksum disagrees with release'
    records = json.loads((root / 'docs/compatibility-records.json').read_text(encoding='utf-8'))
    assert records['current_release'] == tag, 'Compatibility release marker is stale'
    assert f'releases/tag/{tag}' in compat and f'## {tag} ' in changelog, 'Compatibility/changelog release link is stale'
    assert records['source_commit'] in provenance, 'Source revision disagrees with compatibility provenance'
    seen = set()
    for record in records['records']:
        for field in ['game', 'title_id', 'hardware', 'test', 'stage', 'notes', 'section', 'evidence']:
            assert record.get(field), f'Missing compatibility field {field}'
        assert record['game'] not in seen, 'Duplicate game in compatibility records'
        seen.add(record['game'])
        assert '| ' + record['game'] + ' / ' + record['title_id'] + ' |' in compat, 'A recorded title is missing from the public table'
        assert record['evidence'] in records['evidence'], 'Unknown evidence source'
    survey = compat.split('<summary>Show all 28 dated startup results, game versions and observations</summary>', 1)
    assert len(survey) == 2, 'Missing preserved startup survey'
    assert sum(line.startswith('| ') for line in survey[1].split('</details>', 1)[0].splitlines()) == records['startup_survey']['sessions'] + 1, 'Startup survey rows were lost'

    # Resolve every local Markdown link. Historical release links are allowed;
    # current-download assertions above keep primary calls to action up to date.
    checked_links = 0
    for path in root.rglob('*.md'):
        if '.git' in path.parts:
            continue
        text = path.read_text(encoding='utf-8')
        for target in re.findall(r'\]\(([^\s)]+)\)', text):
            parsed = urllib.parse.urlsplit(target)
            if parsed.scheme or target.startswith('#'):
                continue
            resolved = (path.parent / urllib.parse.unquote(parsed.path)).resolve()
            assert resolved.is_relative_to(root.resolve()), f'Local link escapes repository in {path.name}'
            assert resolved.exists(), f'Broken local link in {path.name}: {target}'
            if parsed.fragment and resolved.suffix == '.md':
                headings = re.findall(r'^#+\s+(.+)$', resolved.read_text(encoding='utf-8'), re.M)
                slugs = [re.sub(r'[^\w -]', '', h.lower()).replace(' ', '-') for h in headings]
                assert parsed.fragment in slugs, f'Broken heading link in {path.name}: {target}'
            checked_links += 1
    print(json.dumps({'release': tag, 'required_assets': len(required), 'game_records': len(seen), 'startup_sessions': records['startup_survey']['sessions'], 'local_links_checked': checked_links, 'status': 'passed'}, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    check(parser.parse_args().root)
