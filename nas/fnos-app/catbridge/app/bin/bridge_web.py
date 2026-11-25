#!/usr/bin/env python3
"""bridge-web: NAS-side print console for the CatPrint bridge (Plan B).

Serves one page on :8080 that owns everything print-related:
  - print settings  -> /etc/catbridge.conf (algo/darkness/speed/feed/host)
  - live status     -> proxied from the ESP32 bridge (/api/status) + CUPS queue
  - docs            -> where AirPrint discovery / queue plumbing lives

Stdlib only (http.server + urllib); runs under systemd as catbridge-web.
"""
import json
import os
import subprocess
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs

CONFIG_PATH = "/etc/catbridge.conf"
ESP_TIMEOUT_S = 4

PAGE = """<!doctype html>
<html lang='zh-CN'>
<head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>CatPrint 控制台</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#f4f6f9;color:#1c2430;font:14px/1.5 -apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}
.wrap{max-width:760px;margin:0 auto;padding:16px}
header{display:flex;align-items:center;gap:10px;margin:8px 0 18px}
h1{font-size:20px;margin:0;flex:1}
h2{font-size:15px;margin:0 0 12px;color:#37506e}
.badge{padding:4px 10px;border-radius:20px;background:#e3f5ea;color:#1a8f4c;font-size:12px;font-weight:700}
.badge.bad{background:#fdecec;color:#c0392b}
.card{background:#fff;border:1px solid #e3e8ef;border-radius:14px;padding:16px;margin-bottom:14px;box-shadow:0 1px 3px rgba(0,0,0,.04)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}
.kv{background:#f7f9fc;border-radius:10px;padding:8px 10px}
.kv b{display:block;font-size:11px;color:#78889c;font-weight:500;letter-spacing:.04em}
.kv span{font-size:14px;font-weight:600;word-break:break-all}
label{display:block;margin:10px 0 4px;font-size:12px;color:#55647a}
input,select{width:100%;padding:9px 10px;border:1px solid #d6dde8;border-radius:9px;font-size:14px;background:#fff;color:#1c2430}
button{margin:8px 6px 0 0;padding:9px 14px;border:0;border-radius:9px;background:#2f6fed;color:#fff;font-size:13px;font-weight:600;cursor:pointer}
.hint{font-size:12px;color:#7b8a9e;margin-top:8px}
#toast{position:fixed;left:50%;bottom:24px;transform:translateX(-50%);background:#1f2a3a;color:#fff;padding:10px 16px;border-radius:10px;font-size:13px;opacity:0;transition:opacity .25s;pointer-events:none;max-width:88%;text-align:center;z-index:9}
pre{background:#f7f9fc;border-radius:10px;padding:10px;font-size:12px;overflow-x:auto;margin:8px 0 0}
</style>
</head>
<body>
<div class=wrap>
<header><h1>CatPrint 打印控制台（NAS）</h1><span id=mode class=badge>加载中…</span></header>

<section class=card><h2>实时状态</h2><div id=status class=grid>加载中…</div>
<div class=hint>每 5 秒自动刷新。ESP32 只负责 WiFi/BLE，设备管理请访问它的 <a href='http://__ESP__/' target=_blank>本地页面</a>。</div>
<pre id=queue></pre></section>

<section class=card><h2>打印设置</h2>
<label>二值化算法（catprinter 同名算法）</label>
<select id=algo>
<option value=floyd-steinberg>Floyd-Steinberg 抖动（默认，细节最好）</option>
<option value=atkinson>Atkinson 抖动（更柔和）</option>
<option value=halftone>Halftone 网点（复古报纸感）</option>
<option value=mean-threshold>均值阈值（纯黑白，无抖动）</option>
<option value=none>无二值化（图像需已是 384 宽黑白）</option>
</select>
<label>打印浓度 darkness（0-15，8 为已知稳妥值）</label>
<input id=darkness type=number min=0 max=15>
<label>走纸 speed（finish 阶段 0xBD 参数，8 为默认）</label>
<input id=speed type=number min=1 max=15>
<label>任务末尾走纸 feed（点数，128 为默认）</label>
<input id=feed type=number min=0 max=1024>
<label>ESP32 桥地址</label>
<input id=host placeholder='192.168.1.21'>
<button onclick=save()>保存设置</button>
<div class=hint>保存立即生效（每个打印作业启动时读取配置），无需重启任何服务。任意纸张尺寸都会自动等比缩放到 384 列热敏纸宽并连续拼接。</div>
</section>

<section class=card><h2>链路说明</h2>
<div class=hint>Mac/iPhone → NAS CUPS(:631, AirPrint) → catbridge 后端（本机做解码/缩放/二值化）→ ESP32 :80/raw → BLE 打印。队列管理可用 CUPS 自带 Web：<a href='http://__HOST__:631/printers/CatPrint' target=_blank>http://__HOST__:631/printers/CatPrint</a></div>
</section>

<div id=toast></div>
<script>
var espUrl='http://__ESP__/api/status';
function t(m){var e=document.getElementById('toast');e.textContent=m;e.style.opacity=1;clearTimeout(e._h);e._h=setTimeout(function(){e.style.opacity=0},4000)}
function stName(n){return['空闲','扫描中','连接中','已就绪','打印中','错误'][n]||n}
function load(){
fetch('/api/status').then(function(r){return r.json()}).then(function(j){
var c=j.conf,e=j.esp||{},w=e.wifi||{},b=e.ble||{},s=e.sys||{};
var ok=!!(w.connected&&b.ready);
var m=document.getElementById('mode');m.textContent=ok?'在线':'部分离线';m.className='badge'+(ok?'':' bad');
document.getElementById('status').innerHTML=
'<div class=kv><b>ESP32 WIFI</b><span>'+(w.ssid||'-')+(w.rssi?' · '+w.rssi+'dBm':'')+'</span></div>'+
'<div class=kv><b>ESP32 IP</b><span>'+(w.ip||'-')+'</span></div>'+
'<div class=kv><b>打印机 BLE</b><span>'+(b.stateName||'-')+(b.mac?' · '+b.mac:'')+'</span></div>'+
'<div class=kv><b>ESP32 内存</b><span>'+(s.heap?Math.round(s.heap/1024)+' KB':'-')+'</span></div>'+
'<div class=kv><b>当前算法</b><span>'+c.algo+'</span></div>'+
'<div class=kv><b>浓度</b><span>'+c.darkness+' / 15</span></div>';
document.getElementById('queue').textContent='CUPS 待打队列: '+(j.queue.length?j.queue.join(' | '):'(空)');
algo.value=c.algo;darkness.value=c.darkness;speed.value=c.speed;feed.value=c.feed;host.value=c.host;
}).catch(function(){document.getElementById('mode').textContent='NAS 服务异常'});}
function save(){
var f=new URLSearchParams();
f.set('algo',algo.value);f.set('darkness',darkness.value);f.set('speed',speed.value);
f.set('feed',feed.value);f.set('host',host.value);
fetch('/api/save',{method:'POST',body:f.toString()}).then(function(r){return r.json()}).then(function(j){t(j.msg||'');if(j.ok)load()});
}
load();setInterval(load,5000);
</script>
</div>
</body>
</html>
"""


def read_conf():
    conf = {"host": "192.168.1.21", "algo": "floyd-steinberg",
            "darkness": "8", "speed": "8", "feed": "128"}
    try:
        with open(CONFIG_PATH) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, v = line.split("=", 1)
                    if k.strip() in conf:
                        conf[k.strip()] = v.strip()
    except FileNotFoundError:
        pass
    return conf


def write_conf(vals):
    order = ["host", "algo", "darkness", "speed", "feed"]
    tmp = CONFIG_PATH + ".tmp"
    with open(tmp, "w") as f:
        f.write("# CatPrint bridge settings (edited via NAS console)\n")
        for k in order:
            f.write("%s=%s\n" % (k, vals.get(k, "")))
    os.replace(tmp, CONFIG_PATH)


def esp_status(host):
    try:
        req = urllib.request.Request("http://%s/api/status" % host)
        with urllib.request.urlopen(req, timeout=ESP_TIMEOUT_S) as r:
            return json.loads(r.read().decode())
    except Exception as e:  # noqa: BLE001
        return {"error": str(e)}


def cups_queue():
    try:
        out = subprocess.run(["lpstat", "-o"], capture_output=True,
                             text=True, timeout=5).stdout.strip()
        return [l for l in out.splitlines() if l.strip()]
    except Exception:  # noqa: BLE001
        return ["(lpstat 不可用)"]


def host_ip():
    """LAN IP for doc links. Containers get it via CATBRIDGE_HOST_IP env
    (hostname -I inside a container reports the internal bridge IP)."""
    ip = os.environ.get("CATBRIDGE_HOST_IP")
    if ip:
        return ip
    try:
        out = subprocess.run(["hostname", "-I"], capture_output=True,
                             text=True, timeout=3).stdout.split()
        return out[0] if out else ""
    except Exception:  # noqa: BLE001
        return ""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):  # quiet
        pass

    def _json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            html = PAGE.replace("__ESP__", read_conf()["host"]) \
                       .replace("__HOST__", host_ip())
            body = html.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/api/status":
            conf = read_conf()
            self._json(200, {"conf": conf,
                             "esp": esp_status(conf["host"]),
                             "queue": cups_queue()})
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path != "/api/save":
            self.send_error(404)
            return
        n = int(self.headers.get("Content-Length") or 0)
        form = parse_qs(self.rfile.read(n).decode())
        valid_algos = {"floyd-steinberg", "atkinson", "halftone",
                       "mean-threshold", "none"}
        vals = {
            "host": (form.get("host", [""])[0] or "192.168.1.21").strip(),
            "algo": form.get("algo", ["floyd-steinberg"])[0],
            "darkness": max(0, min(15, int(form.get("darkness", ["8"])[0] or 8))),
            "speed": max(1, min(15, int(form.get("speed", ["8"])[0] or 8))),
            "feed": max(0, min(1024, int(form.get("feed", ["128"])[0] or 128))),
        }
        if vals["algo"] not in valid_algos:
            self._json(400, {"ok": False, "msg": "未知算法"})
            return
        try:
            write_conf(vals)
        except OSError as e:
            self._json(500, {"ok": False, "msg": "写入失败: %s" % e})
            return
        self._json(200, {"ok": True, "msg": "已保存，下个打印作业立即生效",
                         "conf": vals})


def main():
    srv = ThreadingHTTPServer(("0.0.0.0", 8080), Handler)
    print("bridge-web listening on :8080")
    srv.serve_forever()


if __name__ == "__main__":
    main()
