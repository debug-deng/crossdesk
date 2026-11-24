"""Download the artifacts of a GitHub Actions run into deploy/selfhost/dist.

Usage:
    python deploy/selfhost/fetch_artifacts.py <run_id> [dest_dir]

Credentials are read from the git credential helper, so no PAT has to be
stored in this file.
"""

import io
import json
import os
import subprocess
import sys
import urllib.request
import zipfile

REPO = "debug-deng/crossdesk"
DEFAULT_DEST = r"K:\crossdesk\deploy\selfhost\dist"


def token():
    proc = subprocess.run(
        ["git", "credential", "fill"],
        input="protocol=https\nhost=github.com\n\n",
        capture_output=True,
        text=True,
        env={**os.environ, "GIT_TERMINAL_PROMPT": "0"},
        cwd=r"K:\crossdesk",
    )
    for line in proc.stdout.splitlines():
        if line.startswith("password="):
            return line.split("=", 1)[1]
    raise SystemExit("no github credential available")


TOKEN = token()


def get(path, accept="application/vnd.github+json", raw=False):
    req = urllib.request.Request(
        "https://api.github.com" + path,
        headers={
            "Authorization": "token " + TOKEN,
            "Accept": accept,
            "User-Agent": "wb",
        },
    )
    with urllib.request.urlopen(req, timeout=120) as resp:
        data = resp.read()
    return data if raw else json.loads(data)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    """Return the 302 itself so the signed URL can be fetched without auth.

    Artifact downloads redirect to a pre-signed blob URL; forwarding the
    GitHub Authorization header there makes the blob service reject it.
    """

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def download_zip(path):
    opener = urllib.request.build_opener(NoRedirect)
    req = urllib.request.Request(
        "https://api.github.com" + path,
        headers={
            "Authorization": "token " + TOKEN,
            "Accept": "application/vnd.github+json",
            "User-Agent": "wb",
        },
    )
    try:
        with opener.open(req, timeout=120) as resp:
            return resp.read()
    except urllib.error.HTTPError as exc:
        if exc.code not in (301, 302, 303, 307, 308):
            raise
        location = exc.headers["Location"]
    with urllib.request.urlopen(location, timeout=300) as resp:
        return resp.read()


def main():
    run_id = int(sys.argv[1])
    dest = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_DEST
    os.makedirs(dest, exist_ok=True)

    payload = get(f"/repos/{REPO}/actions/runs/{run_id}/artifacts")
    if not payload["artifacts"]:
        raise SystemExit("run has no artifacts")

    for art in payload["artifacts"]:
        print(f"downloading {art['name']} ({art['size_in_bytes']} bytes)")
        blob = download_zip(f"/repos/{REPO}/actions/artifacts/{art['id']}/zip")
        with zipfile.ZipFile(io.BytesIO(blob)) as zf:
            for info in zf.infolist():
                # The artifact zip wraps payloads in directories; keep the
                # trailing components so the portable zip lands unpacked.
                target = os.path.join(dest, info.filename)
                os.makedirs(os.path.dirname(target) or dest, exist_ok=True)
                if info.is_dir():
                    continue
                with zf.open(info) as src, open(target, "wb") as out:
                    out.write(src.read())
                print("  ->", target)


if __name__ == "__main__":
    main()
