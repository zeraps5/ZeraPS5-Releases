#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real Windows sharing locks against an isolated transactional installer.

Optional --baseline/--package use immutable published ZIPs, not mock binaries.
The executing helper is deliberately the candidate under test, not the helper
shipped in those old releases. No existing installation or process is changed.
"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import threading
import time
import zipfile


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--installer', type=Path, required=True)
parser.add_argument('--work', type=Path, required=True)
parser.add_argument('--baseline', type=Path)
parser.add_argument('--package', type=Path)
parser.add_argument('--expect-unfixed', action='store_true')
args = parser.parse_args()
assert bool(args.baseline) == bool(args.package)
args.installer = args.installer.resolve()
work = args.work.resolve()
work.mkdir(parents=True, exist_ok=False)
sha = lambda data: hashlib.sha256(data).hexdigest()
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                              wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
kernel.CreateFileW.restype = wintypes.HANDLE
kernel.CloseHandle.argtypes = [wintypes.HANDLE]
kernel.CloseHandle.restype = wintypes.BOOL
kernel.LockFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD]
kernel.LockFile.restype = wintypes.BOOL
markers = {'user/saves/lock-test.keep': b'synthetic save',
           'user/config/zeraps5.ini': b'[interface]\nidioma=en\n[atualizacao]\nnao_iniciar=1\n',
           'user/config/PPSA00000.ini': b'[synthetic]\nkeep=1\n',
           'games/lock-test.keep': b'synthetic game',
           'system/modules/lock-test.keep': b'synthetic module'}
results = []


def extract(archive, target):
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            assert (target / info.filename).resolve().is_relative_to(target)
        z.extractall(target)
        tops = {name.split('/')[0] for name in z.namelist()}
    assert len(tops) == 1
    return target / tops.pop()


if args.package:
    package = args.package.resolve()
    with zipfile.ZipFile(package) as z:
        manifest_name, = [n for n in z.namelist() if n.endswith('/update-files.json')]
        payload = json.loads(z.read(manifest_name))['files']
else:
    package = work / 'payload.zip'
    files = {'ZeraPS5.exe': b'new launcher', 'ps5c.exe': b'new emulator',
             'tools/apply-update.ps1': args.installer.read_bytes(), 'data/nid_names.txt': b'new data'}
    payload = [{'path': k, 'size': len(v), 'sha256': sha(v)} for k, v in files.items()]
    with zipfile.ZipFile(package, 'w', zipfile.ZIP_DEFLATED) as z:
        for path, data in files.items():
            z.writestr('ZeraPS5-test/' + path, data)
        z.writestr('ZeraPS5-test/update-files.json', json.dumps(
            {'schema': 1, 'build': 999, 'channel': 'stable', 'platform': 'windows-x86_64', 'files': payload}))


def installation(name):
    dest = work / name
    dest.mkdir()
    if args.baseline:
        app = extract(args.baseline, dest)
    else:
        app = dest
        (app / 'ZeraPS5.exe').write_bytes(b'old launcher')
        (app / 'ps5c.exe').write_bytes(b'old emulator')
    for path, data in markers.items():
        p = app / path
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    # Recovery remains outside the payload so replacing tools/apply-update.ps1
    # cannot replace the code executing the transaction under test.
    helper = app / 'user/updates/Recuperar-ZeraPS5.ps1'
    helper.parent.mkdir(parents=True, exist_ok=True)
    helper.write_bytes(args.installer.read_bytes())
    before = {p.relative_to(app).as_posix(): sha(p.read_bytes()) for p in app.rglob('*') if p.is_file()}
    return app, helper, before


def lock(path, share):
    handle = kernel.CreateFileW(str(path), 0x80000000, share, None, 3, 0, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    return handle


def command(app, helper, *opts):
    return ['powershell.exe', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
            '-File', str(helper), '-InstallRoot', str(app), '-Package', str(package),
            '-ExpectedSha256', sha(package.read_bytes()), *opts]


def execute(app, helper, *opts, release=None, wait_for=None):
    env = dict(os.environ, PS5C_UPDATE_TEST_HOOKS='1')
    start = time.monotonic()
    with (app.parent / (app.name + '-stdout.txt')).open('wb') as out, \
         (app.parent / (app.name + '-stderr.txt')).open('wb') as err:
        proc = subprocess.Popen(command(app, helper, *opts), stdout=out, stderr=err, env=env)
        thread = None
        if release is not None:
            def unlock():
                try:
                    deadline = time.monotonic() + 40
                    while wait_for and not wait_for.exists() and proc.poll() is None and time.monotonic() < deadline:
                        time.sleep(.02)
                    time.sleep(2)
                finally:
                    kernel.CloseHandle(release)
            thread = threading.Thread(target=unlock)
            thread.start()
        try:
            code = proc.wait(timeout=150)
        finally:
            if proc.poll() is None:
                proc.kill()  # Only this test's isolated helper, never a user application.
                proc.wait()
            if thread:
                thread.join(timeout=45)
                assert not thread.is_alive()
    return code, time.monotonic() - start


def preserved(app, before, all_files=False):
    for path, digest in before.items():
        if all_files or path in markers:
            assert sha((app / path).read_bytes()) == digest, path
    if all_files:
        for item in payload:
            if item['path'] not in before:
                assert not (app / item['path']).exists(), item['path']


def installed(app):
    for item in payload:
        file = app / item['path']
        assert file.stat().st_size == item['size'] and sha(file.read_bytes()) == item['sha256'], item['path']
    journal = json.loads((app / 'user/updates/transaction/journal.json').read_text(encoding='utf-8-sig'))
    assert journal['phase'] == 'committed'


def record(name, seconds, app, **extra):
    item = dict(test=name, seconds=round(seconds, 3), installation=str(app), **extra)
    results.append(item)
    print(json.dumps(item), flush=True)


lock_modes = [('reader-denies-write', 1), ('exclusive-reader', 0)]
if not args.expect_unfixed:
    lock_modes.append(('byte-range-lock', 7))
for mode, share in lock_modes:
    app, helper, before = installation(mode)
    handle = lock(app / 'ps5c.exe', share)
    if mode == 'byte-range-lock':
        assert kernel.LockFile(handle, 0, 0, 0xffffffff, 0), ctypes.WinError(ctypes.get_last_error())
    code, seconds = execute(app, helper, release=handle, wait_for=app / 'user/updates/transaction/stage')
    preserved(app, before, all_files=args.expect_unfixed)
    if args.expect_unfixed:
        assert code != 0
        assert '80070020' in (app / 'user/updates/result.txt').read_text(encoding='utf-8-sig')
    else:
        assert code == 0, (mode, code)
        installed(app)
        events = [json.loads(line) for line in (app / 'user/updates/file-locks.jsonl').read_text(encoding='utf-8-sig').splitlines()]
        expected_error = 33 if mode == 'byte-range-lock' else 32
        assert any(e['resolved'] and e['attempts'] > 1 and e['win32'] == expected_error for e in events), events
    record(mode, seconds, app, exit_code=code, expected_old_failure=args.expect_unfixed)

if not args.expect_unfixed:
    app, helper, before = installation('readonly-destination')
    readonly = app / 'ps5c.exe'
    readonly.chmod(0o444)
    try:
        code, seconds = execute(app, helper)
        assert code == 0
        installed(app)
        preserved(app, before)
    finally:
        readonly.chmod(0o666)
    record('readonly attribute still supported by force-copy', seconds, app)

    app, helper, before = installation('two-updaters')
    # An actual independent handle represents a first helper waiting on a
    # scanner. A second helper must leave its transaction and backups alone.
    updates = app / 'user/updates'
    lock_file = updates / 'installer.lock'
    lock_file.write_bytes(b'')
    handle = lock(lock_file, 0)
    try:
        code, seconds = execute(app, helper, '-FileLockTimeoutSeconds', '0')
        assert code != 0
        preserved(app, before, all_files=True)
        assert not (updates / 'transaction').exists()
        error = json.loads((updates / 'file-error.json').read_text(encoding='utf-8-sig'))
        assert error['operation'] == 'installer-lock' and error['win32'] == 32
    finally:
        kernel.CloseHandle(handle)
    code, _ = execute(app, helper)
    assert code == 0
    installed(app)
    preserved(app, before)
    record('second updater cannot modify an active installation transaction', seconds, app)

    app, helper, before = installation('permanent-lock')
    handle = lock(app / 'ps5c.exe', 1)
    try:
        code, seconds = execute(app, helper, '-FileLockTimeoutSeconds', '1')
        assert code != 0 and seconds < 15
        preserved(app, before, all_files=True)
        assert not (app / 'user/updates/transaction/journal.json').exists()
        error = json.loads((app / 'user/updates/file-error.json').read_text(encoding='utf-8-sig'))
        assert error['file'] == 'ps5c.exe' and error['win32'] == 32 and error['operation'] == 'preflight', error
    finally:
        kernel.CloseHandle(handle)
    record('permanent lock leaves all original bytes intact before commit', seconds, app)

    for mode in ['recovery-transient-lock', 'recovery-permanent-lock']:
        app, helper, before = installation(mode)
        index, changed = next((n, item) for n, item in enumerate(payload, 1)
                              if item['path'] in before and item['sha256'].lower() != before[item['path']])
        code, _ = execute(app, helper, '-TestInterruptAfter', str(index))
        assert code == 37
        first = changed['path']
        assert sha((app / first).read_bytes()) != before[first], first
        handle = lock(app / first, 1)
        if mode == 'recovery-transient-lock':
            code, seconds = execute(app, helper, '-Recover', release=handle)
            assert code == 0
        else:
            try:
                code, seconds = execute(app, helper, '-Recover', '-FileLockTimeoutSeconds', '1')
                assert code != 0
                journal = json.loads((app / 'user/updates/transaction/journal.json').read_text(encoding='utf-8-sig'))
                assert journal['phase'] == 'applying'
            finally:
                kernel.CloseHandle(handle)
            code, _ = execute(app, helper, '-Recover')
            assert code == 0
        preserved(app, before, all_files=True)
        record(mode, seconds, app, all_original_files_verified=len(before))

report = {'status': 'passed', 'helper': str(args.installer), 'helper_sha256': sha(args.installer.read_bytes()),
          'baseline': str(args.baseline) if args.baseline else None,
          'baseline_sha256': sha(args.baseline.read_bytes()) if args.baseline else None,
          'package_sha256': sha(package.read_bytes()), 'payload_files': len(payload),
          'unmodified_published_zip': bool(args.package), 'original_user_installations_changed': False,
          'old_negative_control': args.expect_unfixed, 'tests': results}
(work / 'RESULT.json').write_text(json.dumps(report, indent=2), encoding='utf8')
print('All', len(results), 'file-lock tests passed.', flush=True)
