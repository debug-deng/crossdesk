import json
import os
import subprocess
import sys
import time
import urllib.request

REPO = "debug-deng/crossdesk"
RUN_ID = int(sys.argv[1]) if len(sys.argv) > 1 else 0
LOG = r"K:\crossdesk\ci-status.log"


def token():
    p = subprocess.run(
        ["git", "credential", "fill"],
        input="protocol=https\nhost=github.com\n\n",
        capture_output=True,
        text=True,
        env={**os.environ, "GIT_TERMINAL_PROMPT": "0"},
        cwd=r"K:\crossdesk",
    )
    for line in p.stdout.splitlines():
        if line.startswith("password="):
            return line.split("=", 1)[1]
    raise SystemExit("no github credential")


TOKEN = token()


def api(path):
    req = urllib.request.Request(
        "https://api.github.com" + path,
        headers={
            "Authorization": "token " + TOKEN,
            "Accept": "application/vnd.github+json",
            "User-Agent": "wb",
        },
    )
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())


def snapshot():
    run = api(f"/repos/{REPO}/actions/runs/{RUN_ID}")
    lines = [f"[{time.strftime('%H:%M:%S')}] run {run['status']} {run['conclusion']}"]
    for job in api(f"/repos/{REPO}/actions/runs/{RUN_ID}/jobs")["jobs"]:
        lines.append(f"  job: {job['name']} {job['status']} {job['conclusion']}")
        for step in job["steps"]:
            if step["status"] != "pending":
                lines.append(
                    f"    - {step['number']} {step['name']}: {step['status']} {step['conclusion']}"
                )
    return run, "\n".join(lines)


def main():
    deadline = time.time() + 45 * 60
    while time.time() < deadline:
        try:
            run, text = snapshot()
        except Exception as exc:  # noqa: BLE001
            text = f"[{time.strftime('%H:%M:%S')}] poll error: {exc}"
            run = None
        with open(LOG, "w", encoding="utf-8") as fh:
            fh.write(text + "\n")
        if run and run["status"] == "completed":
            print(text)
            return
        time.sleep(45)
    print("timeout waiting for run")


if __name__ == "__main__":
    main()
