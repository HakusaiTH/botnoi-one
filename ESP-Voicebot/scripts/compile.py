#!/usr/bin/env python3
"""Compile ESP32-S3 firmware with dummy credentials and report static RAM/flash.

Install Arduino CLI from https://docs.arduino.cc/arduino-cli/installation/.
First run: python3 ESP-Voicebot/scripts/compile.py --install-deps
Later:    python3 ESP-Voicebot/scripts/compile.py

The default toolchain and all artifacts live in ignored ESP-Voicebot/build/.
Use --arduino-cli and --config-file to reuse another CLI installation. This
script never uploads firmware or connects to the voicebot service. Compile-time
RAM use excludes runtime heap, TLS, task stacks, DMA, and PSRAM allocations.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


CORE_VERSION = "3.3.11"
JSON_VERSION = "7.4.3"
BOARD_INDEX = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"
SOURCE_SUFFIXES = {".ino", ".h", ".hpp", ".c", ".cpp", ".S"}
TARGETS = {
    "opi": "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,CDCOnBoot=default",
    "none": "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=disabled,CDCOnBoot=default",
}
DUMMY_CONFIG = """#pragma once
#define WIFI_SSID "compile-test-only"
#define WIFI_PASS "compile-test-only"
#define BOTNOI_API_KEY "compile-test-only"
#define BOTNOI_AGENT_ID "compile-test-only"
#define BOTNOI_WS_HOST "voicebot-stg.botnoigroup.com"
#define BOTNOI_WS_PATH "/v1/preview_call"
#define BOTNOI_WS_PORT 443
"""


def stage_sketch(source, destination):
    """Copy firmware only; neither read nor embed config.local.h credentials."""
    destination.mkdir()
    for item in source.iterdir():
        if item.is_file() and item.suffix in SOURCE_SUFFIXES:
            if item.name not in {"config.local.h", "config.example.h"}:
                shutil.copy2(item, destination / item.name)
        elif item.name == "src" and item.is_dir():
            shutil.copytree(item, destination / "src")
    example = (source / "config.example.h").read_text()
    for name in ("WIFI_SSID", "WIFI_PASS", "BOTNOI_API_KEY", "BOTNOI_AGENT_ID"):
        example = re.sub(r"(^\s*#define\s+" + name + r")\s+[^\n]*", r'\1 "compile-test-only"', example, flags=re.MULTILINE)
    (destination / "config.example.h").write_text(example)
    (destination / "config.local.h").write_text(DUMMY_CONFIG)
    if (source / "partitions.csv").is_file():
        shutil.copy2(source / "partitions.csv", destination / "partitions.csv")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--arduino-cli", default=os.environ.get("ARDUINO_CLI", "arduino-cli"))
    parser.add_argument("--config-file", type=Path, default=os.environ.get("ARDUINO_CONFIG_FILE"))
    parser.add_argument("--install-deps", action="store_true", help="Download pinned official core and ArduinoJson into the selected toolchain")
    parser.add_argument("--target", choices=["all", *TARGETS], default="all")
    parser.add_argument("--aec", choices=["default", "on", "off"], default="default",
                        help="Use firmware default, or explicitly compile AEC on/off (dummy credentials remain enforced)")
    parser.add_argument("--build-root", type=Path, help="Artifact and default isolated toolchain directory")
    parser.add_argument("--sketch", type=Path, default=Path(__file__).resolve().parents[1], help="Sketch source directory (for baseline comparisons)")
    args = parser.parse_args()

    cli = shutil.which(args.arduino_cli)
    if not cli:
        parser.error("Arduino CLI is missing; install it or set --arduino-cli /path/to/arduino-cli")
    sketch = args.sketch.resolve()
    if not (sketch / "ESP-Voicebot.ino").is_file():
        parser.error("--sketch must contain ESP-Voicebot.ino")
    root = (args.build_root or sketch / "build").resolve()
    root.mkdir(parents=True, exist_ok=True)
    config = args.config_file
    if config is None:
        toolchain = root / "toolchain"
        toolchain.mkdir(exist_ok=True)
        config = toolchain / "arduino-cli.yaml"
        if not config.exists():
            config.write_text(json.dumps({
                "board_manager": {"additional_urls": [BOARD_INDEX]},
                "directories": {name: str(toolchain / name) for name in ("data", "downloads", "user")},
                "updater": {"enable_notification": False},
            }, indent=2) + "\n")
    command = [cli, "--config-file", str(config.resolve()), "--no-color"]
    environment = dict(os.environ, ARDUINO_BUILD_CACHE_PATH=str(root / "cache"), ARDUINO_UPDATER_ENABLE_NOTIFICATION="false")

    def run(*arguments, capture=False):
        return subprocess.run(command + list(arguments), check=True, env=environment,
                              text=True, stdout=subprocess.PIPE if capture else None).stdout

    if args.install_deps:
        run("core", "update-index", "--additional-urls", BOARD_INDEX)
        run("core", "install", "esp32:esp32@" + CORE_VERSION, "--additional-urls", BOARD_INDEX)
        run("lib", "install", "ArduinoJson@" + JSON_VERSION)
    cli_version = run("version", capture=True).strip()
    core_list = run("core", "list", capture=True)
    libraries = json.loads(run("lib", "list", "--json", capture=True))
    json_libraries = [item["library"] for item in libraries.get("installed_libraries", [])
                      if item["library"]["name"] == "ArduinoJson"]
    if not re.search(r"^esp32:esp32\s+" + re.escape(CORE_VERSION) + r"\s", core_list, re.MULTILINE):
        parser.error(f"esp32:esp32@{CORE_VERSION} is missing; run with --install-deps")
    if not json_libraries or any(item["version"] != JSON_VERSION for item in json_libraries):
        parser.error(f"Use ArduinoJson@{JSON_VERSION} only in this toolchain; run with --install-deps")
    print(cli_version, flush=True)
    print(f"ESP32 core {CORE_VERSION}; ArduinoJson {JSON_VERSION}", flush=True)

    artifact = Path(tempfile.mkdtemp(prefix="compile-", dir=root))
    staged = artifact / "ESP-Voicebot"
    stage_sketch(sketch, staged)
    summary = {
        "arduino_cli": cli_version, "esp32_core": CORE_VERSION, "arduinojson": JSON_VERSION,
        "credentials": "dummy", "aec": args.aec, "targets": {},
        "staged_source_sha256": {
            str(path.relative_to(staged)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(staged.rglob("*")) if path.is_file()
        },
    }
    results = TARGETS if args.target == "all" else {args.target: TARGETS[args.target]}
    for name, fqbn in results.items():
        print(f"\nBuilding {name}: {fqbn}", flush=True)
        log_path = artifact / f"{name}.log"
        compile_command = command + ["compile", "--fqbn", fqbn, "--warnings", "all",
                                     "--build-path", str(artifact / name)]
        if args.aec != "default":
            compile_command += ["--build-property", "compiler.cpp.extra_flags=-DVOICEBOT_AEC_ENABLED=" +
                                ("1" if args.aec == "on" else "0")]
        compile_command += [str(staged)]
        with log_path.open("w") as log:
            process = subprocess.Popen(compile_command, env=environment, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True)
            for line in process.stdout:
                print(line, end="", flush=True)
                log.write(line)
            return_code = process.wait()
        if return_code:
            print(f"Compile failed; log: {log_path}", file=sys.stderr)
            return return_code
        output = log_path.read_text()
        flash = re.search(r"Sketch uses ([\d,]+) bytes .*?Maximum is ([\d,]+) bytes", output)
        ram = re.search(r"Global variables use ([\d,]+) bytes .*?leaving ([\d,]+) bytes .*?Maximum is ([\d,]+) bytes", output)
        if not flash or not ram:
            print(f"Cannot parse build memory report; inspect {log_path}", file=sys.stderr)
            return 1
        number = lambda value: int(value.replace(",", ""))
        summary["targets"][name] = {
            "fqbn": fqbn,
            "flash_bytes": number(flash[1]), "flash_limit_bytes": number(flash[2]),
            "static_ram_bytes": number(ram[1]), "ram_after_static_bytes": number(ram[2]),
            "ram_limit_bytes": number(ram[3]),
        }
    (artifact / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"\nArtifacts: {artifact}")
    print("Static RAM report excludes runtime TLS, task stacks, DMA, queues, and PSRAM.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
