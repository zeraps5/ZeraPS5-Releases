# SPDX-License-Identifier: GPL-2.0-or-later
"""ZeraPS5 update server for the testers' launchers (LAN today, internet later).

  GET  /latest.json, /<zip>   the published builds (tools/publish.ps1 writes them)
       latest.json?build=N&id=I&pc=P   also records that installation's build in
                                       registro/versoes.csv (one line per check)
  POST /relatorios            a problem report zip from the launcher (by hand, or
                              on its own when a game closes: X-ZeraPS5-Auto: 1);
                              saved in relatorios/pendentes/ (the analysis queue)
                              and listed in relatorios/fila.csv

registro/ and relatorios/ are private: never served back.

  python tools/update_server.py [--dir releases/servidor] [--port 8750]
"""
import argparse
import csv
import datetime
import functools
import http.server
import os
import re
import threading
import urllib.parse

MAX_REPORT = 64 << 20
PRIVATE = ('/registro', '/relatorios')
_lock = threading.Lock()


def safe(text, limit=40):
    return re.sub(r'[^A-Za-z0-9_-]', '_', text or '')[:limit] or 'x'


def append_csv(path, header, row):
    with _lock:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        new = not os.path.exists(path)
        with open(path, 'a', newline='', encoding='utf-8') as f:
            w = csv.writer(f)
            if new:
                w.writerow(header)
            w.writerow(row)


class Handler(http.server.SimpleHTTPRequestHandler):
    def now(self):
        return datetime.datetime.now().isoformat(timespec='seconds')

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        if any(url.path == p or url.path.startswith(p + '/') for p in PRIVATE):
            self.send_error(403)
            return
        if url.path == '/latest.json' and url.query:
            q = urllib.parse.parse_qs(url.query)
            first = lambda k: q.get(k, [''])[0]
            append_csv(os.path.join(self.directory, 'registro', 'versoes.csv'),
                       ['data', 'ip', 'instalacao', 'pc', 'build'],
                       [self.now(), self.client_address[0], safe(first('id'), 32), safe(first('pc')),
                        safe(first('build'), 10)])
        super().do_GET()

    do_HEAD = do_GET

    def do_POST(self):
        if urllib.parse.urlsplit(self.path).path != '/relatorios':
            self.send_error(404)
            return
        size = int(self.headers.get('Content-Length') or 0)
        if size <= 0 or size > MAX_REPORT:
            self.send_error(413)
            return
        data = self.rfile.read(size)
        if not data.startswith(b'PK\x03\x04'):
            self.send_error(400, 'not a zip')
            return
        game = safe(self.headers.get('X-ZeraPS5-Game'))
        build = safe(self.headers.get('X-ZeraPS5-Build'), 10)
        pc = safe(self.headers.get('X-ZeraPS5-Pc'))
        stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
        auto = self.headers.get('X-ZeraPS5-Auto') == '1'  # sent by the launcher when a game closed
        name = f'{stamp}-{pc}-{game}-b{build}{"-auto" if auto else ""}.zip'
        folder = os.path.join(self.directory, 'relatorios', 'pendentes')
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, name), 'wb') as f:
            f.write(data)
        append_csv(os.path.join(self.directory, 'relatorios', 'fila.csv'),
                   ['data', 'arquivo', 'jogo', 'build', 'pc', 'instalacao', 'ip', 'estado'],
                   [self.now(), name, game, build, pc, safe(self.headers.get('X-ZeraPS5-Id'), 32),
                    self.client_address[0], 'pendente-auto' if auto else 'pendente'])
        body = (name + '\n').encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/plain; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        print(f'[{self.now()}] relatório recebido: {name}', flush=True)

    def log_message(self, fmt, *args):  # quiet: only the reports are printed
        pass


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', default=os.path.join(root, 'releases', 'servidor'))
    ap.add_argument('--port', type=int, default=8750)
    a = ap.parse_args()
    handler = functools.partial(Handler, directory=a.dir)
    server = http.server.ThreadingHTTPServer(('0.0.0.0', a.port), handler)
    print(f'ZeraPS5: servindo {a.dir} na porta {a.port} (Ctrl+C encerra)', flush=True)
    server.serve_forever()


if __name__ == '__main__':
    main()
