#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Isolated update tests. No GitHub calls, game launch, or process termination."""
import argparse, contextlib, hashlib, http.server, json, os, pathlib, subprocess, sys, tempfile, threading, zipfile

p=argparse.ArgumentParser();p.add_argument('--build',required=True);p.add_argument('--work',required=True)
a=p.parse_args();build=pathlib.Path(a.build).resolve();work=pathlib.Path(a.work).resolve();work.mkdir(parents=True,exist_ok=True)
root=pathlib.Path(__file__).resolve().parents[1];installer=root/'tools/apply-update.ps1'
sha=lambda b:hashlib.sha256(b).hexdigest()
results=[]
def run(args,success=True):
    env=dict(os.environ,PS5C_UPDATE_TEST_HOOKS='1');env['PATH']='C:/msys64/ucrt64/bin;'+env['PATH']
    r=subprocess.run([str(x) for x in args],env=env,capture_output=True,timeout=150)
    if (r.returncode==0)!=success:raise AssertionError((r.returncode,r.stdout.decode(errors='replace')[-800:],r.stderr.decode(errors='replace')[-1000:]))
    return r
def passed(name):results.append(name);print('PASS',name,flush=True)
def package(dst,name='valid.zip',extra=None,bad_hash=False):
    files={'ZeraPS5.exe':b'new launcher','ps5c.exe':b'new emulator','tools/apply-update.ps1':installer.read_bytes(),'data/nid_names.txt':b'new database'}
    if extra:files.update(extra)
    manifest={'schema':1,'build':999,'channel':'stable','platform':'windows-x86_64','files':[{'path':k,'size':len(v),'sha256':sha(v) if not bad_hash else '0'*64}for k,v in files.items()]}
    with zipfile.ZipFile(dst/name,'w',zipfile.ZIP_DEFLATED)as z:
        for k,v in files.items():z.writestr('ZeraPS5-test/'+k,v)
        z.writestr('ZeraPS5-test/update-files.json',json.dumps(manifest))
    return dst/name
def installation(dst):
    dst.mkdir();old={'ZeraPS5.exe':b'old launcher','ps5c.exe':b'old emulator','user/save.dat':b'my save','user/config/local.ini':b'my settings','games/keep.dat':b'my game','system/modules/keep.dat':b'my module'}
    for k,v in old.items():q=dst/k;q.parent.mkdir(parents=True,exist_ok=True);q.write_bytes(v)
    return old
def verify(dst,old,all_files=False):
    for k,v in old.items():
        if all_files or k.startswith(('user/','games/','system/')):assert (dst/k).read_bytes()==v,k
def apply(dst,z,success=True,*opts):
    return run(['powershell.exe','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',installer,'-InstallRoot',dst,'-Package',z,'-ExpectedSha256',sha(z.read_bytes()),*opts],success)

with tempfile.TemporaryDirectory(prefix='update-tests-',dir=work) as temp:
    t=pathlib.Path(temp);z=package(t)
    dst=t/'success';old=installation(dst);apply(dst,z);verify(dst,old)
    assert (dst/'ps5c.exe').read_bytes()==b'new emulator';passed('apply and preserve saves/config/games/system')
    catalog=json.loads((root/'frontends/launcher/technical_translations.json').read_text(encoding='utf8'))
    messages={x['source']:x['text']for x in catalog if x['kind']=='installer'}
    success_key='Update installed. Previous program files retained in user\\updates\\transaction\\backup.'
    for locale,expected in messages[success_key].items():
        dest=t/('language-'+locale);old=installation(dest)
        pref=b'[interface]\nidioma='+locale.encode()+b'\n[geral]\nidioma=17\n'
        (dest/'user/config/zeraps5.ini').write_bytes(pref);old['user/config/zeraps5.ini']=pref
        apply(dest,z);verify(dest,old)
        actual=(dest/'user/updates/result.txt').read_text(encoding='utf-8-sig').strip()
        assert actual==expected,(locale,actual,expected)
        passed('installer result in '+locale)
    dest=t/'language-failure-fa';old=installation(dest)
    (dest/'user/config/zeraps5.ini').write_text('[interface]\nidioma=fa\n',encoding='utf8')
    apply(dest,z,False,'-TestFailAfter','3');verify(dest,old,True)
    failure_key='Update failed. Program files were restored when possible. Run user\\updates\\Recuperar-ZeraPS5.ps1 with -InstallRoot and -Recover if interrupted.'
    assert messages[failure_key]['fa']in(dest/'user/updates/result.txt').read_text(encoding='utf-8-sig')
    passed('localized failure retains rollback in Persian')
    dst=t/'failure';old=installation(dst);apply(dst,z,False,'-TestFailAfter','3');verify(dst,old,True)
    assert not(dst/'tools/apply-update.ps1').exists();passed('copy failure rolls back old and newly added files')
    dst=t/'interrupted';old=installation(dst);apply(dst,z,False,'-TestInterruptAfter','2')
    assert json.loads((dst/'user/updates/transaction/journal.json').read_text(encoding='utf-8-sig'))['phase']=='applying'
    run(['powershell.exe','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',installer,'-InstallRoot',dst,'-Recover'])
    verify(dst,old,True);passed('interrupted install recovers from durable journal')
    for name,extra,badhash in [('protected',{'user/save.dat':b'destroy'},False),('traversal',{'../../escaped.txt':b'escape'},False),('bad-inner-hash',None,True),('case-collision',{'PS5C.EXE':b'duplicate'},False)]:
        dest=t/name;old=installation(dest);bad=package(t,name+'.zip',extra,badhash);apply(dest,bad,False);verify(dest,old,True);passed('reject '+name)
    dest=t/'bad-zip-hash';old=installation(dest)
    run(['powershell.exe','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',installer,'-InstallRoot',dest,'-Package',z,'-ExpectedSha256','0'*64],False)
    verify(dest,old,True);passed('reject outer checksum mismatch')
    dest=t/'wrong-build';old=installation(dest);apply(dest,z,False,'-ExpectedBuild','1000');verify(dest,old,True);passed('reject payload build mismatch')
    # Exercise actual Windows sharing/byte-range locks, including recovery and
    # an active installer lock. Keep this in the normal distribution gate.
    lock_work=t/'file-lock-tests'
    run([sys.executable,root/'tools/test_update_file_locks.py','--installer',installer,'--work',lock_work])
    for test in json.loads((lock_work/'RESULT.json').read_text(encoding='utf8'))['tests']:
        passed('Windows file locks: '+test['test'])
    # Native WinHTTP, same parser and SHA implementation the launcher ships.
    requests=[]
    notes=json.loads((root/'frontends/launcher/translations_extra.json').read_text(encoding='utf8'))['Versão experimental. Consulte LEIA-ME e limitações conhecidas.']
    manifest={'schema':1,'channel':'stable','platform':'windows-x86_64','build':999,'full_version':'local-test','zip':'ZeraPS5-test.zip','zip_sha256':sha(z.read_bytes()),'notes_localized':notes}
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_GET(self):
            requests.append(self.path)
            if self.path.startswith('/redirect/'):
                self.send_response(302);self.send_header('Location','/latest.json');self.end_headers();return
            if self.path in ('/latest.json','/bad/latest.json','/arch/latest.json','/preview/latest.json'):
                m=dict(manifest)
                if self.path.startswith('/bad/'):m['zip']='../escaped.zip'
                if self.path.startswith('/arch/'):m['platform']='linux-x86_64'
                if self.path.startswith('/preview/'):m['channel']='preview'
                data=json.dumps(m).encode();self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data);return
            if self.path in ('/403/latest.json','/429/latest.json','/missing/latest.json'):
                self.send_error(429 if '429' in self.path else 403 if '403' in self.path else 404);return
            if self.path=='/ZeraPS5-test.zip':
                data=z.read_bytes();self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data);return
            if self.path=='/partial.zip':
                self.send_response(200);self.send_header('Content-Length','500000');self.end_headers();self.wfile.write(b'partial');self.close_connection=True;return
            self.send_error(404)
    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
    try:
        url='http://127.0.0.1:'+str(server.server_port);native=build/'test_launcher_update.exe';cwd=t/'client';cwd.mkdir()
        run([native]);passed('native report privacy')
        out=run([native,'query',url,cwd]);assert out.stdout.startswith(b'999|ZeraPS5-test.zip|');passed('native version query')
        for locale,expected in notes.items():
            out=run([native,'query-language',url,cwd,locale])
            assert out.stdout.decode('utf8').strip()==expected,locale
            passed('native update notes in '+locale)
        run([native,'query',url+'/redirect',cwd]);passed('native release redirect')
        for key in ['bad','arch','preview','403','429','missing']:run([native,'query',url+'/'+key,cwd],False);passed('native reject '+key)
        download=t/'download.zip';run([native,'download',url+'/ZeraPS5-test.zip',download]);assert download.read_bytes()==z.read_bytes()
        out=run([native,'sha',download]);assert out.stdout.strip().decode()==sha(z.read_bytes());passed('native download and SHA256')
        run([native,'download',url+'/partial.zip',t/'partial.zip'],False);assert not(t/'partial.zip').exists();passed('incomplete download removed')
        assert all('?'not in v for v in requests);passed('no computer identity or query parameters sent')
    finally:server.shutdown();server.server_close();thread.join()
(work/'distribution-tests.json').write_text(json.dumps({'passed':len(results),'tests':results},indent=2))
print('All',len(results),'distribution tests passed.')
