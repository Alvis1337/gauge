"""Self-update for the Pi app: `git fetch` + `git pull --ff-only` against
origin/main, since deploys here are just a git checkout with no separate
build/release step (see config._detect_version's docstring)."""
import logging
import os
import subprocess

log = logging.getLogger("updater")

_REPO_DIR = os.path.dirname(os.path.abspath(__file__))
_BRANCH = "main"


def _git(*args, timeout=30):
    return subprocess.run(
        ["git", *args], cwd=_REPO_DIR, timeout=timeout,
        capture_output=True, text=True, check=False,
    )


def _rev_parse(ref: str) -> str:
    out = _git("rev-parse", ref)
    return out.stdout.strip() if out.returncode == 0 else ""


def check_and_apply(status_holder: list) -> bool:
    """Runs one check-for-update cycle, writing a short state string to
    status_holder[0] as it progresses ("checking", "updating", "restarting",
    "up_to_date", or "error"). Returns True if a new commit was pulled,
    meaning the caller should restart the process to run it."""
    status_holder[0] = "checking"
    fetch = _git("fetch", "origin", _BRANCH)
    if fetch.returncode != 0:
        log.warning("update fetch failed: %s", fetch.stderr.strip())
        status_holder[0] = "error"
        return False

    local, remote = _rev_parse("HEAD"), _rev_parse(f"origin/{_BRANCH}")
    if not remote or local == remote:
        status_holder[0] = "up_to_date"
        return False

    status_holder[0] = "updating"
    pull = _git("pull", "--ff-only", "origin", _BRANCH)
    if pull.returncode != 0:
        log.warning("update pull failed: %s", pull.stderr.strip())
        status_holder[0] = "error"
        return False

    log.info("updated %s -> %s, restarting", local[:7], remote[:7])
    status_holder[0] = "restarting"
    return True
