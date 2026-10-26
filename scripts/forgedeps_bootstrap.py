"""Zero-setup entry for project build scripts (standard library only).

A project copies this file byte for byte as ``scripts/forgedeps_bootstrap.py``
and calls ``ensure()`` before importing ForgeDeps::

    import forgedeps_bootstrap
    forgedeps_bootstrap.ensure(ROOT / "requirements-build.txt")

``ensure`` returns when the running interpreter already provides the exact
tooling pinned by the requirements file (CI and developer environments). In
every other case it prepares one shared virtual environment for those pins
below the ForgeDeps workspace and runs the same command again with it, so a
fresh clone needs only Python and ``python scripts/build.py``. CMake and Ninja
are added to that environment when they are not on PATH; tools that need an
administrator (a C/C++ compiler, platform SDKs, Qt) are reported with the
command that installs them. Set FORGEDEPS_BOOTSTRAP=0 to disable all of this.

This module never imports ForgeDeps: it runs before ForgeDeps is installed.
"""
from __future__ import annotations

import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
import venv
from importlib import metadata
from pathlib import Path

MINIMUM_PYTHON = (3, 14)
_REENTRY = "FORGEDEPS_BOOTSTRAPPED"
_PIN = re.compile(r"^\s*([A-Za-z0-9_.-]+)\s*(?:@\s*\S+@v?(\d[\w.]*)|==\s*(\d[\w.]*))\s*$")
_PIP_TOOLS = {"cmake": "cmake", "ninja": "ninja"}
_LOCK_STALE_SECONDS = 30 * 60


class BootstrapError(SystemExit):
    """A setup problem the user has to resolve; the message says how."""

    def __init__(self, message: str):
        super().__init__("forgedeps bootstrap: " + message)


def pinned_versions(requirements: Path) -> dict[str, str]:
    """Distribution name -> exact version for every pinned requirement line."""
    pins = {}
    for line in requirements.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        match = _PIN.match(line)
        if not match:
            raise BootstrapError(f"{requirements}: requirement is not pinned to one version: {line!r}")
        pins[_canonical(match.group(1))] = match.group(2) or match.group(3)
    if not pins:
        raise BootstrapError(f"{requirements}: no pinned requirements")
    return pins


def satisfied(pins: dict[str, str]) -> bool:
    """True when this interpreter already has exactly the pinned distributions."""
    for name, version in pins.items():
        try:
            if metadata.version(name) != version:
                return False
        except metadata.PackageNotFoundError:
            return False
    return True


def workspace_home(environ=None) -> tuple[Path, bool]:
    """Return the ForgeDeps workspace and whether it still has to be registered.

    Mirrors forgedeps.home.resolve_home: FORGEDEPS_HOME, then the per-user
    registry written by ``forgedeps init``, then ~/.forgedeps for a first run.
    """
    environ = os.environ if environ is None else environ
    explicit = environ.get("FORGEDEPS_HOME")
    if explicit:
        return Path(explicit).expanduser().resolve(), False
    if sys.platform == "win32":
        base = Path(environ.get("APPDATA") or Path.home() / "AppData/Roaming")
    else:
        base = Path(environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
    registry = base / "forgedeps/workspace.json"
    if registry.is_file() and not registry.is_symlink():
        try:
            workspace = json.loads(registry.read_text(encoding="utf-8")).get("workspace")
        except (OSError, ValueError, AttributeError):
            workspace = None
        if isinstance(workspace, str) and Path(workspace).is_absolute():
            return Path(workspace), False
    return (Path.home() / ".forgedeps").resolve(), True


def tools_environment(home: Path, requirements_text: str) -> Path:
    """One environment per (pins, interpreter, machine) shared by every project."""
    identity = "\n".join((requirements_text.strip(), platform.python_version(),
                          sys.platform, platform.machine().lower()))
    key = hashlib.sha256(identity.encode("utf-8")).hexdigest()[:16]
    return home / "tools" / f"python{sys.version_info[0]}.{sys.version_info[1]}-{key}"


def environment_python(root: Path) -> Path:
    return root / ("Scripts/python.exe" if sys.platform == "win32" else "bin/python")


def environment_scripts(root: Path) -> Path:
    return root / ("Scripts" if sys.platform == "win32" else "bin")


def compiler_hint() -> str | None:
    """Return an install command when no C/C++ compiler is visible, else None."""
    if sys.platform == "darwin":
        found = subprocess.run(["xcrun", "--find", "clang++"], capture_output=True).returncode == 0 \
            if shutil.which("xcrun") else False
        return None if found else "xcode-select --install"
    if sys.platform == "win32":
        program_files = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
        vswhere = Path(program_files) / "Microsoft Visual Studio/Installer/vswhere.exe"
        if vswhere.is_file() or shutil.which("cl") or shutil.which("g++") or shutil.which("clang++"):
            return None
        return ("winget install Microsoft.VisualStudio.BuildTools --override "
                "\"--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive\"")
    if shutil.which("c++") or shutil.which("g++") or shutil.which("clang++"):
        return None
    if shutil.which("apt-get"):
        return "sudo apt-get install -y build-essential"
    if shutil.which("dnf"):
        return "sudo dnf install -y gcc-c++ make"
    if shutil.which("pacman"):
        return "sudo pacman -S --needed base-devel"
    return "install a C++23 compiler (GCC 16+ or Clang 23+)"


def ensure(requirements, *, tools=("cmake", "ninja"), argv=None) -> None:
    """Return with pinned ForgeDeps importable, or re-run the caller so it is."""
    if os.environ.get("FORGEDEPS_BOOTSTRAP") == "0":
        return
    if sys.version_info < MINIMUM_PYTHON:
        wanted = ".".join(map(str, MINIMUM_PYTHON))
        raise BootstrapError(f"Python {wanted}+ is required, this is {platform.python_version()} "
                             f"({sys.executable}). Install it from python.org or with "
                             f"`uv python install {wanted}`, then run the same command with it.")
    requirements = Path(requirements).resolve()
    pins = pinned_versions(requirements)
    home, unregistered = workspace_home()
    root = tools_environment(home, requirements.read_text(encoding="utf-8"))
    hint = compiler_hint()
    if hint:
        raise BootstrapError(f"no C/C++ compiler was found. Install one with:\n    {hint}")
    if satisfied(pins):
        _add_missing_tools(root, tools)
        return
    if os.environ.get(_REENTRY):
        raise BootstrapError(f"the prepared environment {os.environ[_REENTRY]} still does not provide "
                             f"{', '.join(f'{name}=={version}' for name, version in pins.items())}")
    _prepare(root, requirements, pins)
    python = environment_python(root)
    if unregistered:
        _run([python, "-m", "forgedeps", "init", str(home)], "register the ForgeDeps workspace")
    _add_missing_tools(root, tools)
    _reenter(python, argv)


def _prepare(root: Path, requirements: Path, pins: dict[str, str]) -> None:
    marker = root / ".forgedeps-ready"
    if marker.is_file():
        return
    root.parent.mkdir(parents=True, exist_ok=True)
    with _Lock(root.with_name(root.name + ".lock")):
        if marker.is_file():
            return
        if root.exists():
            shutil.rmtree(root)  # an interrupted earlier attempt
        print(f"forgedeps bootstrap: preparing {root}", file=sys.stderr, flush=True)
        _create_environment(root)
        python = environment_python(root)
        if not shutil.which("git"):
            raise BootstrapError("git is required to install the pinned build tooling")
        _run([python, "-m", "pip", "install", "--disable-pip-version-check", "--quiet",
              "-r", str(requirements)], "install " + ", ".join(sorted(pins)))
        marker.write_text(json.dumps(pins, sort_keys=True) + "\n", encoding="utf-8")


def _add_missing_tools(root: Path, tools) -> None:
    """Install pip-distributed build tools that are not on PATH; prepend them."""
    missing = [name for name in tools if name in _PIP_TOOLS and not shutil.which(name)]
    if not missing:
        return
    python = environment_python(root)
    if not python.is_file():
        print(f"forgedeps bootstrap: preparing {root}", file=sys.stderr, flush=True)
        root.parent.mkdir(parents=True, exist_ok=True)
        with _Lock(root.with_name(root.name + ".lock")):
            if not python.is_file():
                _create_environment(root)
    scripts = environment_scripts(root)
    absent = [name for name in missing if not shutil.which(name, path=str(scripts))]
    if absent:
        _run([python, "-m", "pip", "install", "--disable-pip-version-check", "--quiet",
              *(_PIP_TOOLS[name] for name in absent)], "install " + ", ".join(absent))
    os.environ["PATH"] = str(scripts) + os.pathsep + os.environ.get("PATH", "")


def _create_environment(root: Path) -> None:
    # Same policy as `python -m venv`: copying the interpreter on POSIX breaks
    # builds that locate libpython relative to the executable (macOS, uv).
    venv.EnvBuilder(with_pip=True, symlinks=os.name != "nt").create(root)


def _reenter(python: Path, argv) -> None:
    command = [str(python), *(sys.argv if argv is None else argv)]
    environment = dict(os.environ, **{_REENTRY: str(python)})
    sys.stdout.flush()
    sys.stderr.flush()
    if sys.platform == "win32":
        raise SystemExit(subprocess.run(command, env=environment).returncode)
    os.execve(command[0], command, environment)


def _run(command, purpose: str) -> None:
    result = subprocess.run([str(part) for part in command])
    if result.returncode != 0:
        raise BootstrapError(f"could not {purpose} (exit {result.returncode})")


def _canonical(name: str) -> str:
    return re.sub(r"[-_.]+", "-", name).lower()


class _Lock:
    """Cross-process exclusive lock based on atomic directory creation."""

    def __init__(self, path: Path):
        self.path = path

    def __enter__(self):
        while True:
            try:
                self.path.mkdir()
                return self
            except FileExistsError:
                try:
                    if time.time() - self.path.stat().st_mtime > _LOCK_STALE_SECONDS:
                        self.path.rmdir()
                        continue
                except FileNotFoundError:
                    continue
                time.sleep(1)

    def __exit__(self, *exc):
        try:
            self.path.rmdir()
        except FileNotFoundError:
            pass
        return False
