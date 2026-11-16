"""Build a review fixture from hash-pinned, already-public b984 source materials."""
import hashlib, io, json, os, shutil, urllib.request, zipfile
from pathlib import Path

BASELINE = 'https://github.com/zeraps5/ZeraPS5-Releases/releases/download/v0.1.1-b984/ZeraPS5-v0.1.1-b984-win64.zip'
WINDOWS_SHA = '46b513f07ddb78c3def09a3e80038f50468968ee122db2b1922b7ed2c8171eb0'
SOURCE_SHA = '814bbadf56ab3064d279076df90d69823e5730ff8b43aa24dc66c6675ed7ec81'
FFMPEG_SHA = '089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304'
repo = Path(os.environ['GITHUB_WORKSPACE']).resolve()
work = Path(os.environ['RUNNER_TEMP']).resolve() / 'zeraps5-signpath'
assert not work.exists(), 'Use a fresh runner working directory'
work.mkdir()
request = urllib.request.Request(BASELINE, headers={'User-Agent': 'ZeraPS5-signing-readiness'})
with urllib.request.urlopen(request, timeout=120) as response:
    windows = response.read()
assert hashlib.sha256(windows).hexdigest() == WINDOWS_SHA
with zipfile.ZipFile(io.BytesIO(windows)) as z:
    prefix = 'ZeraPS5-v0.1.1-b984-win64/sources/'
    sources = z.read(prefix+'ZeraPS5-source.zip')
    ffmpeg = z.read(prefix+'ffmpeg-7.1.2.tar.xz')
assert hashlib.sha256(sources).hexdigest() == SOURCE_SHA
assert hashlib.sha256(ffmpeg).hexdigest() == FFMPEG_SHA
source = work / 'source'
source.mkdir()
with zipfile.ZipFile(io.BytesIO(sources)) as z:
    for info in z.infolist():
        assert (source/info.filename).resolve().is_relative_to(source)
    z.extractall(source)
assert not (source/'.git').exists()
ffmpeg_dir = source/'third_party/ffmpeg'
ffmpeg_dir.mkdir(parents=True, exist_ok=True)
(ffmpeg_dir/'ffmpeg-7.1.2.tar.xz').write_bytes(ffmpeg)
resource_hashes = {'frontends/launcher/launcher.rc':'cc6ce0c4f90920665dfdcd2810250bfd02349dc1330592cb367dea079c7269ec','frontends/cli/ps5c.rc':'154a19297e83fa00212f21e8f5269bb5a0fad8f55ca11689f036aa1c7b50566b'}
for rel, expected in resource_hashes.items():
    path = source/rel
    assert hashlib.sha256(path.read_bytes()).hexdigest() == expected
    launcher = '/launcher/' in rel
    name = 'ZeraPS5' if launcher else 'ps5c'
    description = 'ZeraPS5 launcher' if launcher else 'ZeraPS5 emulation runtime'
    text = path.read_text(encoding='utf8')
    text += f'\n#define ZERA_RESOURCE_DESCRIPTION "{description}"\n#define ZERA_RESOURCE_INTERNAL_NAME "{name}"\n#define ZERA_RESOURCE_ORIGINAL_FILENAME "{name}.exe"\n#include "../common/version_info.rc"\n'
    path.write_text(text, encoding='utf8')
common = source/'frontends/common'
common.mkdir(exist_ok=True)
shutil.copyfile(repo/'.github/scripts/signpath/version_info.rc', common/'version_info.rc')
(source/'LOCAL-REVIEW.txt').write_text('Unsigned CI readiness fixture: public b984 sources plus Windows version-resource metadata. Not a new public release; not a signed or bit-identical b984 binary.\n', encoding='utf8')
provenance = {'purpose':'unsigned-signing-readiness-only','workflow_commit':os.environ['GITHUB_SHA'],'baseline_release':'v0.1.1-b984','windows_sha256':WINDOWS_SHA,'source_sha256':SOURCE_SHA,'ffmpeg_sha256':FFMPEG_SHA,'source_commit':'0d269bb8de1cdc4dce31f4d689d8fef0d675d700','changes':['Windows VERSIONINFO resource metadata only','LOCAL-REVIEW marker; dirty version label retained'],'production_signature':False,'published_release_replaced':False}
(work/'ci-provenance.json').write_text(json.dumps(provenance,indent=2),encoding='utf8')
with open(os.environ['GITHUB_ENV'],'a',encoding='utf8') as f:
    f.write(f'ZERA_SOURCE_DIR={source}\nZERA_REVIEW_DIR={work}\n')
print(json.dumps(provenance,indent=2))
