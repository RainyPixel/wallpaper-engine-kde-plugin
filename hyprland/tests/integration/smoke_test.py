#!/usr/bin/env python3
"""Wayland smoke test for wallpaper-engine-hyprland.

Starts the host with the repository fixture on one output of the running session,
checks page load, animation, properties, page reload and navigation handling,
pause/freeze/resume, recovery from a renderer exit while paused, the singleton lock and
shutdown. It creates a real background surface for about a minute. Layer checks run
only when hyprctl can reach a Hyprland instance.

Exit codes: 0 passed, 1 failed, 77 skipped (no Wayland session).
"""

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIXTURE = HERE.parent / "fixtures" / "web-basic"
NAMESPACE = "wallpaper-engine-hyprland"
SKIP = 77


class SmokeFailure(Exception):
    pass


def require(condition, message):
    if not condition:
        raise SmokeFailure(message)


class Host:
    def __init__(self, binary, instance, log_dir):
        self.binary = binary
        self.instance = instance
        self.log_dir = log_dir
        self.proc = None
        self.log_path = None
        self.status = {}

    def start(self, project, output, name):
        self.log_path = self.log_dir / f"{name}.log"
        log = self.log_path.open("w")
        # Without a controlling terminal Qt may log to the systemd journal instead of stderr.
        env = dict(os.environ, QT_FORCE_STDERR_LOGGING="1")
        self.proc = subprocess.Popen(
            [self.binary, "run", str(project), "--output", output,
             "--instance", self.instance, "--diagnostics", "--duration", "240"],
            stdout=log, stderr=subprocess.STDOUT, env=env)
        log.close()

    def log_tail(self, lines=40):
        if not self.log_path or not self.log_path.exists():
            return ""
        return "\n".join(self.log_path.read_text(errors="replace").splitlines()[-lines:])

    def control(self, command, *extra, timeout=15):
        result = subprocess.run(
            [self.binary, command, *extra, "--instance", self.instance],
            capture_output=True, text=True, timeout=timeout)
        data = None
        if result.returncode == 0 and result.stdout.strip():
            data = json.loads(result.stdout)
        return result.returncode, data, result.stderr.strip()

    def trigger(self, action):
        """Makes the fixture page run an action through its "trigger" property."""
        value = json.dumps({"trigger": f"{action}:{time.monotonic_ns()}"})
        code, _, err = self.control("set-properties", value)
        require(code == 0, f"trigger {action} failed with {code}: {err}")

    def wait_for(self, predicate, what, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise SmokeFailure(f"host exited with {self.proc.returncode} while waiting "
                                   f"for {what}\n{self.log_tail()}")
            code, data, _ = self.control("status")
            if code == 0:
                self.status = data
                if predicate(data):
                    return data
            time.sleep(0.25)
        raise SmokeFailure(f"timed out waiting for {what}; last status: "
                           f"{json.dumps(self.status)[:2000]}\n{self.log_tail()}")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


def surface(status):
    return (status.get("surfaces") or [{}])[0]


def page(status):
    return (surface(status).get("diagnostics") or {}).get("page") or {}


def renderer_processes(root_pid):
    """Chromium renderer processes below the host process, found through /proc."""
    children, cmdlines = {}, {}
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text()
            ppid = int(stat.rsplit(")", 1)[1].split()[1])
            cmdline = (entry / "cmdline").read_bytes().split(b"\0")
        except (OSError, ValueError, IndexError):
            continue
        pid = int(entry.name)
        children.setdefault(ppid, []).append(pid)
        cmdlines[pid] = cmdline
    found, pending, seen = [], [root_pid], set()
    while pending:
        for child in children.get(pending.pop(), []):
            if child in seen:
                continue
            seen.add(child)
            pending.append(child)
            if b"--type=renderer" in cmdlines.get(child, []):
                found.append(child)
    return found


def runtime_file(instance, suffix):
    return Path(os.environ["XDG_RUNTIME_DIR"]) / NAMESPACE / f"{instance}{suffix}"


def hyprland_layers():
    if not os.environ.get("HYPRLAND_INSTANCE_SIGNATURE") or not shutil.which("hyprctl"):
        return None
    try:
        return json.loads(subprocess.check_output(["hyprctl", "-j", "layers"], timeout=5))
    except (OSError, subprocess.SubprocessError, ValueError):
        return None


def namespaces_by_level(layers, monitor):
    levels = (layers.get(monitor) or {}).get("levels") or {}
    return {level: [entry.get("namespace") for entry in entries]
            for level, entries in levels.items()}


def pick_output(binary, requested):
    result = subprocess.run([binary, "outputs"], capture_output=True, text=True, timeout=30)
    require(result.returncode == 0, f"'outputs' failed: {result.stderr.strip()}")
    names = [entry["name"] for entry in json.loads(result.stdout)]
    require(names, "no outputs reported")
    if requested:
        require(requested in names, f"output {requested} not connected (have {names})")
        return requested, names
    if shutil.which("hyprctl") and os.environ.get("HYPRLAND_INSTANCE_SIGNATURE"):
        try:
            monitors = json.loads(subprocess.check_output(["hyprctl", "-j", "monitors"],
                                                          timeout=5))
            for monitor in monitors:
                if monitor.get("focused") and monitor.get("name") in names:
                    return monitor["name"], names
        except (OSError, subprocess.SubprocessError, ValueError):
            pass
    return names[0], names


def run(args, report, work):
    binary = str(Path(args.binary).resolve())
    output, names = pick_output(binary, args.output)
    report["output"] = output
    report["outputs"] = names

    project = work / "web fixture #1"
    shutil.copytree(FIXTURE, project)
    instance = f"smoke-{os.getpid()}"
    wait = args.timeout

    host = Host(binary, instance, work)
    second = Host(binary, instance, work)
    try:
        host.start(project, output, "host")
        status = host.wait_for(lambda s: s.get("allLoaded"), "page load", wait)
        require(status["mode"] == "layer", "host is not using a layer surface")
        require([s["output"] for s in status["surfaces"]] == [output],
                f"unexpected surfaces {status['surfaces']}")
        require(status["missingOutputs"] == [], "missing outputs reported")
        report["loaded"] = True

        first = host.wait_for(lambda s: page(s).get("frames", 0) > 0, "animation frames", wait)
        frames = page(first)["frames"]
        elapsed = page(first)["elapsed"]
        later = host.wait_for(lambda s: page(s).get("frames", 0) > frames + 5
                              and page(s).get("elapsed", 0) > elapsed, "animation progress", wait)
        require(page(later)["fps"] == 30, "general properties were not applied")
        require(page(later)["propertyCalls"] >= 1, "user properties were not applied")
        require(page(later)["settings"]["speed"] == 1, "initial speed not received")
        report["animation"] = {"frames": page(later)["frames"], "elapsed": page(later)["elapsed"]}

        layers = hyprland_layers()
        if layers is not None:
            levels = namespaces_by_level(layers, output)
            require(NAMESPACE in levels.get("1", []),
                    f"namespace not on bottom layer of {output}: {levels}")
            for other in names:
                if other != output:
                    others = namespaces_by_level(layers, other)
                    require(all(NAMESPACE not in v for v in others.values()),
                            f"surface also appears on {other}")
            report["layers"] = {"bottom": True, "background": levels.get("0", [])}
            if args.expect_background:
                require(args.expect_background in levels.get("0", []),
                        f"{args.expect_background} not found on the background layer")
        else:
            report["layers"] = "skipped (no Hyprland IPC)"

        code, data, _ = host.control("set-properties", '{"speed": 1.5, "shape": {"value": "square"}}')
        require(code == 0 and sorted(data["changed"]) == ["shape", "speed"],
                f"set-properties failed with {code}")
        host.wait_for(lambda s: page(s).get("settings", {}).get("speed") == 1.5
                      and page(s)["settings"]["shape"] == "square", "property update", wait)
        for bad in ('{"speed": 99}', '{"unknown": 1}', '{"showgrid": "yes"}'):
            code, _, err = host.control("set-properties", bad)
            require(code == 7 and err, f"invalid properties {bad} returned {code}")
        report["properties"] = True

        # Page-initiated reload: the surface state is reset and properties are sent again.
        current = host.wait_for(lambda s: page(s).get("documentId"), "document id", wait)
        document = page(current)["documentId"]
        host.trigger("reload")
        reloaded = host.wait_for(
            lambda s: s["allLoaded"] and page(s).get("documentId") not in (None, document),
            "reloaded page", wait)
        require(page(reloaded)["settings"]["speed"] == 1.5
                and page(reloaded)["settings"]["shape"] == "square",
                "properties were not applied again after the reload")
        require(surface(reloaded)["recoveries"] == 0, "the reload was counted as a recovery")
        document = page(reloaded)["documentId"]

        # Navigation outside the project is refused and the page keeps running.
        host.trigger("navigate-outside")
        refused = host.wait_for(lambda s: surface(s).get("blockedNavigations", 0) >= 1,
                                "blocked navigation", wait)
        require(surface(refused)["lastBlockedNavigation"].endswith("/outside-the-project.html"),
                f"unexpected blocked URL {surface(refused)['lastBlockedNavigation']}")
        time.sleep(1.5)
        blocked = host.wait_for(lambda s: s["allLoaded"] and page(s).get("documentId"),
                                "page after blocked navigation", wait)
        require(page(blocked)["documentId"] == document,
                "blocked navigation replaced the page")
        require(surface(blocked)["recoveries"] == 0,
                "blocked navigation was treated as a failed load")

        # A failed load after the first success loads the project entry again.
        host.trigger("navigate-missing")
        recovered = host.wait_for(
            lambda s: s["allLoaded"] and surface(s).get("recoveries") == 1
            and page(s).get("documentId") not in (None, document),
            "recovery after a failed load", wait)
        require(page(recovered)["href"].endswith("/index.html"),
                f"recovery loaded {page(recovered)['href']} instead of the project entry")
        require(page(recovered)["settings"]["speed"] == 1.5,
                "properties were not applied after the recovery")
        require(surface(recovered)["error"] == "", "error still reported after the recovery")
        report["navigation"] = {"reload": True, "blocked": True, "failedLoadRecovered": True}

        second.start(project, output, "second")
        try:
            code = second.proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            raise SmokeFailure("second host with the same instance did not exit")
        require(code == 5, f"second host exited with {code}, expected 5\n{second.log_tail()}")
        report["singleton"] = True

        before = page(host.status)
        code, _, _ = host.control("pause")
        require(code == 0, "pause failed")
        host.wait_for(lambda s: s["paused"] and s["allFrozen"], "frozen page", wait)
        time.sleep(2)
        code, data, _ = host.control("status")
        require(code == 0 and data["allFrozen"], "page did not stay frozen")
        code, _, _ = host.control("set-properties", '{"label": "while paused"}')
        require(code == 0, "set-properties while paused failed")

        code, _, _ = host.control("resume")
        require(code == 0, "resume failed")
        resumed = host.wait_for(
            lambda s: not s["paused"] and not s["allFrozen"]
            and page(s).get("resumeEvents", 0) >= 1
            and page(s).get("settings", {}).get("label") == "while paused",
            "resumed page", wait)
        after = page(resumed)
        require(after["freezeEvents"] >= 1, "page did not receive a freeze event")
        require(after["pauseCalls"] >= 1, "setPaused(true) was not delivered")
        require(not after["paused"], "setPaused(false) was not delivered")
        host.wait_for(lambda s: page(s).get("frames", 0) > after["frames"] + 5,
                      "animation after resume", wait)
        report["freeze_resume"] = {
            "freezeEvents": after["freezeEvents"],
            "resumeEvents": after["resumeEvents"],
            "ticksAcrossPause": after["ticks"] - before.get("ticks", 0),
        }

        # Renderer exit while paused and frozen: the page is loaded again, then paused and
        # frozen once more.
        renderers = renderer_processes(host.proc.pid)
        if not renderers:
            report["renderer_recovery"] = "skipped (no renderer process found)"
        else:
            document = page(host.status).get("documentId")
            code, _, _ = host.control("pause")
            require(code == 0, "pause failed")
            host.wait_for(lambda s: s["paused"] and s["allFrozen"],
                          "frozen page before the renderer exit", wait)
            recoveries = surface(host.status)["recoveries"]
            killed = 0
            for pid in renderers:
                try:
                    os.kill(pid, signal.SIGKILL)
                    killed += 1
                except (ProcessLookupError, PermissionError):
                    pass
            if not killed:
                report["renderer_recovery"] = "skipped (renderer process could not be signalled)"
            else:
                host.wait_for(lambda s: surface(s).get("recoveries") == recoveries + 1,
                              "renderer exit", wait)
                host.wait_for(lambda s: s["paused"] and s["allLoaded"] and s["allFrozen"],
                              "reloaded page frozen while paused", wait)
            code, _, _ = host.control("resume")
            require(code == 0, "resume failed")
            if killed:
                revived = host.wait_for(
                    lambda s: not s["paused"] and s["allLoaded"] and not s["allFrozen"]
                    and page(s).get("documentId") not in (None, document)
                    and page(s).get("resumeEvents", 0) >= 1,
                    "resumed page after the renderer exit", wait)
                require(page(revived)["pauseCalls"] >= 1 and page(revived)["freezeEvents"] >= 1,
                        "pause was not applied to the reloaded page")
                require(page(revived)["settings"]["label"] == "while paused",
                        "properties were not applied after the renderer exit")
                report["renderer_recovery"] = True

        code, _, _ = host.control("quit")
        require(code == 0, "quit failed")
        require(host.proc.wait(timeout=20) == 0, f"host exit code {host.proc.returncode}")
        require(not runtime_file(instance, ".sock").exists(), "socket left behind")
        require(not runtime_file(instance, ".lock").exists(), "lock file left behind")
        code, _, _ = host.control("status")
        require(code == 6, f"status after quit returned {code}, expected 6")
        if layers is not None:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                current = hyprland_layers() or {}
                if all(NAMESPACE not in v for v in namespaces_by_level(current, output).values()):
                    break
                time.sleep(0.2)
            else:
                raise SmokeFailure("layer surface still present after quit")
        report["quit"] = True

        host.start(project, output, "sigterm")
        host.wait_for(lambda s: s.get("allLoaded"), "page load before SIGTERM", wait)
        host.proc.send_signal(signal.SIGTERM)
        require(host.proc.wait(timeout=20) == 0, f"SIGTERM exit code {host.proc.returncode}")
        require(not runtime_file(instance, ".sock").exists(), "socket left behind after SIGTERM")
        report["sigterm"] = True
    finally:
        second.stop()
        host.stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", default=os.environ.get("WEHYPR_BINARY"),
                        help="path to wallpaper-engine-hyprland (or WEHYPR_BINARY)")
    parser.add_argument("--output", default=os.environ.get("WEHYPR_TEST_OUTPUT"),
                        help="output to use (default: focused Hyprland monitor or first output)")
    parser.add_argument("--expect-background", metavar="NAMESPACE",
                        help="require this namespace on the background layer, "
                             "e.g. omarchy-background")
    parser.add_argument("--timeout", type=float, default=30, help="seconds per wait step")
    parser.add_argument("--report", help="write the JSON report to this file")
    args = parser.parse_args()

    if not args.binary:
        parser.error("--binary is required")
    if not os.environ.get("WAYLAND_DISPLAY") or not os.environ.get("XDG_RUNTIME_DIR"):
        print("skipped: no Wayland session", file=sys.stderr)
        return SKIP

    report = {"result": "FAIL"}
    code = 1
    with tempfile.TemporaryDirectory(prefix="wehypr-smoke-") as tmp:
        try:
            run(args, report, Path(tmp))
            report["result"] = "PASS"
            code = 0
        except (SmokeFailure, subprocess.SubprocessError, OSError, KeyError, TypeError,
                ValueError) as exc:
            report["error"] = f"{type(exc).__name__}: {exc}"
    text = json.dumps(report, indent=2)
    print(text)
    if args.report:
        Path(args.report).write_text(text + "\n")
    return code


if __name__ == "__main__":
    sys.exit(main())
