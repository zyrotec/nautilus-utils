# Smart-HTTP git server with basic auth (user "u", password "tok") for test-gt-git (PLAN.md §14.5 item 5).
# Usage: authserver.py ROOT  — serves the bare repositories in ROOT on 127.0.0.1, prints the port, runs until killed.
import base64, os, subprocess, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = sys.argv[1]
GOOD = 'Basic ' + base64.b64encode(b'u:tok').decode()

class Handler(BaseHTTPRequestHandler):
    def serve(self, method):
        if self.headers.get('Authorization', '') != GOOD:
            self.send_response(401)
            self.send_header('WWW-Authenticate', 'Basic realm="test"')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        path, _, query = self.path.partition('?')
        body = self.rfile.read(int(self.headers.get('Content-Length') or 0)) if method == 'POST' else b''
        env = dict(os.environ, GIT_PROJECT_ROOT=ROOT, GIT_HTTP_EXPORT_ALL='1', PATH_INFO=path, QUERY_STRING=query,
                   REQUEST_METHOD=method, CONTENT_TYPE=self.headers.get('Content-Type', ''),
                   CONTENT_LENGTH=str(len(body)), REMOTE_USER='u')
        out = subprocess.run(['git', 'http-backend'], input=body, env=env, capture_output=True).stdout
        head, _, rest = out.partition(b'\r\n\r\n')
        status, headers = 200, []
        for line in head.split(b'\r\n'):
            key, _, value = line.decode().partition(': ')
            if key.lower() == 'status':
                status = int(value.split()[0])
            elif key:
                headers.append((key, value))
        self.send_response(status)
        for key, value in headers:
            self.send_header(key, value)
        self.send_header('Content-Length', str(len(rest)))
        self.end_headers()
        self.wfile.write(rest)

    def do_GET(self):
        self.serve('GET')

    def do_POST(self):
        self.serve('POST')

    def log_message(self, *args):
        pass

server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
print(server.server_address[1], flush=True)
server.serve_forever()
