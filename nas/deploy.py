#!/usr/bin/env python3
"""Deploy nas/ to the fnOS NAS and run setup.sh via sudo."""
import os
import sys
import paramiko

NAS_HOST = "192.168.1.17"
NAS_USER = "dja"
NAS_PASS = "<REDACTED>"
LOCAL_DIR = os.path.dirname(os.path.abspath(__file__))
REMOTE_DIR = "/tmp/catbridge-deploy"


def run(ssh, cmd, timeout=90):
    chan = ssh.get_transport().open_session()
    chan.settimeout(timeout)
    chan.exec_command(cmd)
    out = b""
    while True:
        if chan.recv_ready():
            out += chan.recv(4096)
        if chan.recv_stderr_ready():
            out += chan.recv_stderr(4096)
        if chan.exit_status_ready() and not chan.recv_ready() and not chan.recv_stderr_ready():
            break
    rc = chan.recv_exit_status()
    return rc, out.decode("utf-8", "replace")


def upload_dir(sftp, local, remote):
    os.makedirs  # noqa
    for root, dirs, files in os.walk(local):
        rel = os.path.relpath(root, local)
        rdir = remote if rel == "." else remote + "/" + rel
        try:
            sftp.stat(rdir)
        except FileNotFoundError:
            sftp.mkdir(rdir)
        for f in files:
            lp = os.path.join(root, f)
            sftp.put(lp, rdir + "/" + f)
            print("  uploaded %s -> %s/%s" % (lp, rdir, f))


def main():
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(NAS_HOST, username=NAS_USER, password=NAS_PASS, timeout=15)
    sftp = ssh.open_sftp()

    print("== uploading nas/ -> %s" % REMOTE_DIR)
    run(ssh, "rm -rf %s" % REMOTE_DIR)
    upload_dir(sftp, LOCAL_DIR, REMOTE_DIR)
    sftp.close()

    print("== running setup.sh (sudo)")
    rc, out = run(ssh,
                  "echo %s | sudo -S -p '' sh %s/setup.sh" % (NAS_PASS, REMOTE_DIR),
                  timeout=600)
    print(out)
    print("== setup rc=%d" % rc)

    print("== post-check")
    for cmd in ["lpstat -p CatPrint",
                "avahi-browse -rt _ipp._tcp 2>/dev/null | head -8",
                "ls -l /usr/lib/cups/backend/catbridge"]:
        rc, out = run(ssh, cmd, timeout=60)
        print("$ %s\n%s" % (cmd, out.strip() or "(no output)"))
    ssh.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
