"""Supervise the two processes launched by the PowerShell faculty demo."""
import json
import os
from pathlib import Path
import subprocess
import sys
import threading

PROJECT = Path(__file__).resolve().parents[2]


def run_backend(session, requested, port):
    import uvicorn

    os.environ.update(APP_PORT=str(port), RAID_STORAGE_ROOT=str(session / "storage"),
                      RAID_ENGINE_EXECUTABLE=str(PROJECT / "build/msvc-debug/Debug/raid_simulator.exe"))
    server = uvicorn.Server(uvicorn.Config("backend.app.main:app", host="127.0.0.1", port=port))
    finished = threading.Event()

    def watch():
        while not finished.wait(0.15):
            if requested():
                server.should_exit = True
                return

    monitor = threading.Thread(target=watch)
    monitor.start()
    try:
        server.run()
        return 0 if server.started else 1
    finally:
        finished.set()
        monitor.join()


def run_frontend(backend_port):
    env = {**os.environ, "VITE_PROXY_TARGET": f"http://127.0.0.1:{backend_port}", "VITE_API_BASE_URL": ""}
    npm = os.environ["FINAL_DEMO_NPM"]
    return subprocess.call([npm, "run", "dev", "--", "--host", "127.0.0.1", "--strictPort",
                            "--config", "browser/faculty-vite.config.js"],
                           cwd=PROJECT / "frontend", env=env)


def main(kind):
    session = Path(os.environ["FINAL_DEMO_ROOT"])
    run = os.environ["FINAL_DEMO_RUN"]
    identity = json.loads((session / "demo.json").read_text(encoding="utf-8"))
    marker = session / f"{kind}-{run}.stop"

    def requested():
        try:
            command = json.loads(marker.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return False
        return command == {"run": run, "demo_token": identity["token"]}

    exit_code = 1
    try:
        if kind == "backend":
            exit_code = run_backend(session, requested, int(os.environ["FINAL_DEMO_PORT"]))
        elif kind == "frontend":
            exit_code = run_frontend(int(os.environ["FINAL_DEMO_BACKEND_PORT"]))
        else:
            raise ValueError("Unknown faculty demo service")
        return exit_code
    finally:
        with (session / "logs" / f"{kind}-{run}.exit.json").open("x", encoding="utf-8") as output:
            json.dump({"run": run, "exit_code": exit_code}, output)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))

