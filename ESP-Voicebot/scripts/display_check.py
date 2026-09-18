#!/usr/bin/env python3
"""Compile a standalone ESP32-S3 ILI9341 color test; never upload firmware.

Uses the production panel driver and config.example.h pin map, with a 1 MHz
default SPI clock. Does not read config.local.h or include Wi-Fi/audio code.
Requires an existing Arduino CLI installation and ESP32 core 3.3.11.
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
FQBN = "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=disabled,CDCOnBoot=default"


def sanitized_config(path):
    text = path.read_text()
    for name in ("WIFI_SSID", "WIFI_PASS", "BOTNOI_API_KEY", "BOTNOI_AGENT_ID"):
        text = re.sub(r"(^\s*#define\s+" + name + r")\s+[^\n]*",
                      r'\1 "display-test-unused"', text, flags=re.MULTILINE)
    return text.encode()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    source = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frequency", type=int, default=1000000,
                        help="SPI write frequency in Hz, from 1000000 to 10000000 (default: 1000000)")
    parser.add_argument("--arduino-cli", default=os.environ.get("ARDUINO_CLI", "arduino-cli"))
    parser.add_argument("--config-file", type=Path, default=os.environ.get("ARDUINO_CONFIG_FILE"))
    args = parser.parse_args()
    if not 1000000 <= args.frequency <= 10000000:
        parser.error("--frequency must be between 1000000 and 10000000 Hz")
    cli = shutil.which(args.arduino_cli)
    if not cli:
        parser.error("Arduino CLI is missing; pass --arduino-cli /path/to/arduino-cli")
    config = args.config_file
    if config is None:
        local_toolchain = source / "build/toolchain/arduino-cli.yaml"
        if local_toolchain.is_file():
            config = local_toolchain
    if config is not None and not config.is_file():
        parser.error("--config-file must name an existing Arduino CLI configuration")
    command = [cli, "--no-color"]
    if config is not None:
        command += ["--config-file", str(config.resolve())]
    root = source / "build"
    root.mkdir(exist_ok=True)
    environment = dict(os.environ, ARDUINO_BUILD_CACHE_PATH=str(root / "cache"),
                       ARDUINO_UPDATER_ENABLE_NOTIFICATION="false")

    def capture(*arguments):
        return subprocess.run(command + list(arguments), check=True, env=environment,
                              text=True, stdout=subprocess.PIPE).stdout

    cli_version = capture("version").strip()
    if not re.search(r"^esp32:esp32\s+" + re.escape(CORE_VERSION) + r"\s",
                     capture("core", "list"), re.MULTILINE):
        parser.error(f"ESP32 core {CORE_VERSION} is missing from the selected toolchain")

    fixture = source / "tests/display_target/display_target.ino"
    driver = source / "display_ili9341.h"
    example = source / "config.example.h"
    inputs = {
        "DisplayCheck.ino": fixture.read_bytes(),
        "display_ili9341.h": driver.read_bytes(),
        "config.example.h": sanitized_config(example),
    }
    artifact = Path(tempfile.mkdtemp(prefix="display-check-", dir=root))
    staged = artifact / "DisplayCheck"
    staged.mkdir()
    for name, data in inputs.items():
        (staged / name).write_bytes(data)
    (staged / "display_check_settings.h").write_text(
        "#pragma once\n// SPI speed for this standalone test, including Arduino IDE uploads.\n"
        f"#define DISPLAY_CHECK_SPI_HZ {args.frequency}UL\n")
    summary = {
        "arduino_cli": cli_version, "esp32_core": CORE_VERSION, "fqbn": FQBN,
        "frequency_hz": args.frequency, "credentials": "unused placeholders",
        "uploaded": False, "hardware_tested": False,
        "sketch": str(staged / "DisplayCheck.ino"),
        "staged_source_sha256": {path.name: digest(path.read_bytes()) for path in sorted(staged.iterdir())},
    }
    print(cli_version, flush=True)
    print(f"ESP32 core {CORE_VERSION}; display SPI {args.frequency} Hz; PSRAM disabled", flush=True)
    print(f"Arduino IDE sketch: {staged / 'DisplayCheck.ino'}", flush=True)
    compile_command = command + ["compile", "--fqbn", FQBN, "--warnings", "all",
                                 "--build-path", str(artifact / "output"), str(staged)]
    summary["compile_command"] = compile_command
    log_path = artifact / "compile.log"
    with log_path.open("w") as log:
        process = subprocess.Popen(compile_command, env=environment, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
        for line in process.stdout:
            print(line, end="", flush=True)
            log.write(line)
        return_code = process.wait()
    output = log_path.read_text()
    summary["compile_returncode"] = return_code
    summary["warnings"] = [line for line in output.splitlines() if "warning:" in line]
    summary["source_matches_workspace"] = inputs == {
        "DisplayCheck.ino": fixture.read_bytes(), "display_ili9341.h": driver.read_bytes(),
        "config.example.h": sanitized_config(example),
    }
    flash = re.search(r"Sketch uses ([\d,]+) bytes .*?Maximum is ([\d,]+) bytes", output)
    ram = re.search(r"Global variables use ([\d,]+) bytes .*?leaving ([\d,]+) bytes .*?Maximum is ([\d,]+) bytes", output)
    if flash and ram:
        number = lambda value: int(value.replace(",", ""))
        summary["memory"] = {
            "flash_bytes": number(flash[1]), "flash_limit_bytes": number(flash[2]),
            "static_ram_bytes": number(ram[1]), "ram_after_static_bytes": number(ram[2]),
            "ram_limit_bytes": number(ram[3]),
        }
    summary["binary_sha256"] = {
        str(path.relative_to(artifact)): digest(path.read_bytes())
        for path in sorted((artifact / "output").glob("DisplayCheck.ino.*"))
        if path.suffix in {".bin", ".elf"}
    }
    (artifact / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"Artifacts: {artifact}")
    print(f"Compiler warnings: {len(summary['warnings'])}; source unchanged: {summary['source_matches_workspace']}")
    print("Compile only. Open the generated sketch in Arduino IDE to review pins and upload manually.")
    if not summary["source_matches_workspace"]:
        print("Source changed during the build; rerun to compile the latest driver/config.", file=sys.stderr)
        return return_code or 1
    return return_code


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
