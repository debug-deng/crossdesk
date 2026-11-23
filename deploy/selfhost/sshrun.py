#!/usr/bin/env python3
"""极简 SSH 执行器（密码认证），用于把本地脚本推到服务器上跑。

用法:
  python sshrun.py HOST USER PASSWORD --cmd "uname -a"
  python sshrun.py HOST USER PASSWORD --script deploy.sh --upload deploy.sh /root/deploy.sh
"""
import argparse
import sys

import paramiko


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("user")
    ap.add_argument("password")
    ap.add_argument("--port", type=int, default=22)
    ap.add_argument("--cmd", help="远程执行的命令")
    ap.add_argument("--script", help="本地脚本文件，内容作为远程命令执行")
    ap.add_argument("--upload", nargs=2, action="append", metavar=("LOCAL", "REMOTE"))
    ap.add_argument("--timeout", type=int, default=1800, help="整体命令超时秒数")
    a = ap.parse_args()

    cli = paramiko.SSHClient()
    cli.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        cli.connect(
            a.host,
            port=a.port,
            username=a.user,
            password=a.password,
            timeout=20,
            banner_timeout=40,
            auth_timeout=40,
            look_for_keys=False,
            allow_agent=False,
        )
    except Exception as exc:  # noqa: BLE001
        print(f"[connect-failed] {type(exc).__name__}: {exc}")
        return 255

    if a.upload:
        sftp = cli.open_sftp()
        for local, remote in a.upload:
            sftp.put(local, remote)
            print(f"[upload] {local} -> {remote}")
        sftp.close()

    if a.script:
        with open(a.script, "r", encoding="utf-8", newline="\n") as fh:
            cmd = fh.read()
    else:
        cmd = a.cmd or ""

    if not cmd.strip():
        cli.close()
        return 0

    chan = cli.get_transport().open_session()
    chan.settimeout(a.timeout)
    # pty 让 stdout/stderr 合并成一条流，避免输出乱序；代价是带 \r
    chan.get_pty()
    chan.exec_command(cmd)

    while True:
        if chan.recv_ready():
            sys.stdout.write(chan.recv(65536).decode("utf-8", "replace").replace("\r", ""))
            sys.stdout.flush()
        elif chan.exit_status_ready() and not chan.recv_ready():
            break
        else:
            chan.status_event.wait(0.2)

    # 收尾：把缓冲区里剩余的内容读干净
    while chan.recv_ready():
        sys.stdout.write(chan.recv(65536).decode("utf-8", "replace").replace("\r", ""))
    code = chan.recv_exit_status()
    print(f"\n[exit={code}]")
    cli.close()
    return code


if __name__ == "__main__":
    sys.exit(main())
