#!/bin/sh
# Plan B deploy script: run ON THE NAS (Debian 12 / 飞牛 fnOS, sudo required).
#
# Installs CUPS + Avahi, the catbridge backend, the CatPrint queue and the
# AirPrint advertisement. Idempotent: safe to re-run.
#
#   scp -r nas/ dja@192.168.1.17:/tmp/catbridge-deploy
#   ssh dja@192.168.1.17
#   echo '<密码>' | sudo -S -p '' sh /tmp/catbridge-deploy/setup.sh
#
# After this, phones/Macs on 192.168.1.* discover "CatPrint" via AirPrint and
# jobs land on the ESP32 BLE bridge at 192.168.1.21.
set -eu

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
BRIDGE_HOST="${CATBRIDGE_HOST:-192.168.1.21}"
LAN_RANGE="192.168.1.0/24"

echo "== 1/6 apt packages (cups, avahi)"
if command -v cupsd >/dev/null 2>&1; then
    echo "    cupsd already installed"
else
    apt-get install -y cups avahi-daemon avahi-utils
fi

echo "== 2/6 cupsd.conf: listen on LAN + allow 192.168.1.0/24"
CONF=/etc/cups/cupsd.conf
cp -n "$CONF" "$CONF.pre-catbridge" 2>/dev/null || true
if ! grep -q "^Listen 0.0.0.0:631" "$CONF"; then
    sed -i '/^Listen /d; /^Port /d' "$CONF"
    sed -i '1i Listen 0.0.0.0:631\nListen /run/cups/cups.sock' "$CONF"
fi
# Allow LAN clients everywhere cupsd restricts access (web UI + printer IPP).
python3 - "$CONF" "$LAN_RANGE" <<'PY'
import re, sys
conf, lan = sys.argv[1], sys.argv[2]
text = open(conf).read()
def add_allow(m):
    block = m.group(0)
    if "Allow" in block:
        return block
    return block.replace("</Location>", "  Allow %s\n</Location>" % lan)
text = re.sub(r"<Location[^>]*>.*?</Location>", add_allow, text, flags=re.S)
open(conf, "w").write(text)
PY

echo "== 3/6 catbridge backend -> /usr/lib/cups/backend/"
install -m 755 "$REPO_DIR/cups-backend/catbridge" /usr/lib/cups/backend/catbridge

echo "== 4/6 CatPrint queue (PPD passthrough image/urf)"
# Fresh state each run: remove an old queue with the same name first.
lpadmin -x CatPrint 2>/dev/null || true
lpadmin -p CatPrint -E -v "catbridge://$BRIDGE_HOST" -P "$REPO_DIR/cups/CatPrint.ppd"
cupsenable CatPrint
cupsaccept CatPrint

echo "== 5/6 Avahi AirPrint service"
install -m 644 "$REPO_DIR/avahi/airprint.service" /etc/avahi/services/airprint.service

echo "== 6/6 restart services"
systemctl restart cups
systemctl restart avahi-daemon

echo
echo "Done. Verify:"
echo "  cupsctl --show-config | grep -i listen"
echo "  lpstat -p CatPrint"
echo "  avahi-browse -rt _ipp._tcp"
echo "  echo test | lp -d CatPrint   (needs the ESP32 bridge online)"
