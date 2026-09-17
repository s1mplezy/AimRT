from __future__ import annotations

import argparse
import errno
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time


def terminate_and_reap(processes: list[subprocess.Popen[str]]) -> None:
    # Stop the traffic-producing client before the server. This keeps normal
    # endpoint disposal out of the business assertion window while retaining
    # one cleanup path for every exit condition.
    for process in reversed(processes):
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                pass

    for process in processes:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
    for process in processes:
        process.wait(timeout=3.0)

    for process in processes:
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            continue
        except OSError as error:
            if error.errno == errno.ESRCH:
                continue
            raise
        raise AssertionError(f"residual process group after cleanup: {process.pid}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server-script", required=True)
    parser.add_argument("--client-script", required=True)
    parser.add_argument("--expect-stream", choices=("server", "client"), required=True)
    parser.add_argument("--expect-text", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()

    server_script = Path(args.server_script).resolve()
    client_script = Path(args.client_script).resolve()
    if not server_script.is_file() or not client_script.is_file():
        raise AssertionError("DDS example launch scripts are missing from the build tree")

    work_dir = Path(args.work_dir).resolve()
    if work_dir.exists():
        shutil.rmtree(work_dir)
    work_dir.mkdir(parents=True)
    logs = {
        "server": work_dir / "server.log",
        "client": work_dir / "client.log",
    }
    processes: list[subprocess.Popen[str]] = []
    interrupted = False

    def handle_signal(_signum: int, _frame: object) -> None:
        nonlocal interrupted
        interrupted = True

    previous_handlers = {
        sig: signal.signal(sig, handle_signal) for sig in (signal.SIGINT, signal.SIGTERM)
    }
    streams = []
    succeeded = False
    try:
        server_log = logs["server"].open("w", encoding="utf-8")
        streams.append(server_log)
        processes.append(
            subprocess.Popen(
                [str(server_script)],
                cwd=server_script.parent,
                stdout=server_log,
                stderr=subprocess.STDOUT,
                text=True,
                start_new_session=True,
            )
        )
        time.sleep(1.0)
        if processes[0].poll() is not None:
            raise AssertionError(f"server exited early: \n{logs['server'].read_text(encoding='utf-8')}")

        client_log = logs["client"].open("w", encoding="utf-8")
        streams.append(client_log)
        processes.append(
            subprocess.Popen(
                [str(client_script)],
                cwd=client_script.parent,
                stdout=client_log,
                stderr=subprocess.STDOUT,
                text=True,
                start_new_session=True,
            )
        )

        deadline = time.monotonic() + 30.0
        expected_log = logs[args.expect_stream]
        while time.monotonic() < deadline:
            if interrupted:
                raise AssertionError("DDS example smoke launcher was interrupted")
            for name, process in zip(("server", "client"), processes):
                if process.poll() is not None:
                    raise AssertionError(
                        f"{name} exited before the business assertion: \n"
                        f"{logs[name].read_text(encoding='utf-8')}"
                    )
            if args.expect_text in expected_log.read_text(encoding="utf-8"):
                succeeded = True
                break
            time.sleep(0.1)
        if not succeeded:
            raise AssertionError(
                f"internal watchdog expired without {args.expect_text!r}\n"
                f"server log: \n{logs['server'].read_text(encoding='utf-8')}\n"
                f"client log: \n{logs['client'].read_text(encoding='utf-8')}"
            )
    finally:
        terminate_and_reap(processes)
        for stream in streams:
            stream.close()
        for sig, handler in previous_handlers.items():
            signal.signal(sig, handler)

    for name, log_path in logs.items():
        content = log_path.read_text(encoding="utf-8")
        for forbidden in ("[Warn]", "[Error]", "[Fatal]", " ERROR]", " FATAL]"):
            if forbidden in content:
                raise AssertionError(f"unexpected diagnostic in {name} log: {forbidden}\n{content}")


if __name__ == "__main__":
    main()
