#!/usr/bin/env python3
"""Compile/run actual ESP-SR DSP fixtures on Espressif's ESP32-S3 QEMU.

Requires the pinned Arduino core (compile.py --install-deps) and Espressif's
qemu-system-xtensa, not upstream QEMU. No firmware is uploaded to a board and
no network connection or credentials are used by the fixture. --build-only
leaves a normal Arduino sketch/binaries which can be run manually on a board.
Emulated processing time is diagnostic, not an ESP32-S3 real-time benchmark.
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
import time

CORE_VERSION = "3.3.11"
QEMU_VERSION = "esp_develop_9.2.2_20260417"


def main():
    sketch = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--arduino-cli', default=os.environ.get('ARDUINO_CLI', 'arduino-cli'))
    parser.add_argument('--config-file', type=Path, default=os.environ.get('ARDUINO_CONFIG_FILE', sketch / 'build/toolchain/arduino-cli.yaml'))
    parser.add_argument('--qemu', default=os.environ.get('QEMU_XTENSA', 'qemu-system-xtensa'))
    parser.add_argument('--psram', choices=['opi', 'none'], default='opi')
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--timeout', type=int, default=600)
    parser.add_argument('--progress-timeout', type=int, default=30,
                        help='Fail after this many seconds without fixture progress (default: 30)')
    args = parser.parse_args()
    cli = shutil.which(args.arduino_cli)
    if not cli:
        parser.error('Arduino CLI missing; pass --arduino-cli')
    qemu = shutil.which(args.qemu)
    if not qemu and not args.build_only:
        parser.error('Espressif QEMU missing; pass --qemu or use --build-only')
    command = [cli, '--config-file', str(args.config_file.resolve()), '--no-color']
    core_list = subprocess.check_output(command + ['core', 'list'], text=True)
    if not re.search(r'^esp32:esp32\s+' + re.escape(CORE_VERSION) + r'\s', core_list, re.MULTILINE):
        parser.error('Install pinned esp32:esp32@' + CORE_VERSION)
    version = subprocess.check_output([qemu, '--version'], text=True).splitlines()[0] if qemu else 'not run'
    if not args.build_only and QEMU_VERSION not in version:
        parser.error('Use pinned Espressif QEMU ' + QEMU_VERSION + '; found ' + version)
    root = sketch / 'build'
    root.mkdir(exist_ok=True)
    artifact = Path(tempfile.mkdtemp(prefix='aec-target-', dir=root))
    staged = artifact / 'aec_target'
    staged.mkdir()
    sources = {name: sketch / 'tests/aec_target' / name for name in ('aec_target.ino', 'aec_fixture.h')}
    sources['echo_canceller.h'] = sketch / 'echo_canceller.h'
    for name, source in sources.items():
        shutil.copy2(source, staged / name)
    summary = {'esp32_core': CORE_VERSION, 'qemu': version, 'psram': args.psram,
               'hardware_tested': False, 'cases': [], 'initialization_memory': [],
               'source_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(staged.iterdir())}}
    fqbn = ('esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,'
            'PSRAM=' + ('opi' if args.psram == 'opi' else 'disabled') + ',CDCOnBoot=default')
    build = artifact / 'firmware'
    compile_command = command + ['compile', '--fqbn', fqbn, '--warnings', 'all', '--build-path', str(build)]
    if args.psram == 'none':
        compile_command += ['--build-property', 'compiler.cpp.extra_flags=-DAEC_TEST_NO_PSRAM=1']
    compile_command += [str(staged)]
    print('Artifacts:', artifact, flush=True)
    with (artifact / 'compile.log').open('w') as log:
        process = subprocess.Popen(compile_command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for line in process.stdout:
            log.write(line)
            print(line, end='', flush=True)
        if process.wait():
            return process.returncode
    if args.build_only:
        (artifact / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
        print('Compiled only; DSP tests were not executed.')
        return 0
    merged = build / 'aec_target.ino.merged.bin'
    if merged.stat().st_size != 16 * 1024 * 1024:
        # Arduino's merged image can omit trailing erased flash; QEMU requires
        # an image matching a supported chip capacity.
        with merged.open('ab') as flash:
            flash.write(b'\xff' * (16 * 1024 * 1024 - merged.stat().st_size))
    uart_path = artifact / 'qemu.log'
    # The file chardev also works when the invoking terminal has no stdin/TTY;
    # QEMU's stdio backend can silently drop UART output in that environment.
    run_command = [qemu, '-machine', 'esp32s3', '-display', 'none', '-monitor', 'none',
                   '-serial', 'file:' + str(uart_path),
                   '-drive', 'file=' + str(merged) + ',if=mtd,format=raw', '-no-reboot']
    if args.psram == 'opi':
        run_command += ['-m', '8M', '-global', 'driver=ssi_psram,property=is_octal,value=true']
    print('Running real ESP-SR fixture; emulated timing is not hardware timing.', flush=True)
    emulator_log = (artifact / 'emulator.log').open('wb')
    process = subprocess.Popen(run_command, stdout=emulator_log, stderr=subprocess.STDOUT)
    pending = b''
    done = None
    deadline = time.monotonic() + args.timeout
    last_progress = time.monotonic()
    failure_reason = 'overall timeout before AEC_DONE'
    try:
        uart_path.touch(exist_ok=True)
        with uart_path.open('rb') as uart:
            while time.monotonic() < deadline and done is None:
                chunk = uart.read(65536)
                if chunk:
                    pending += chunk
                    while b'\n' in pending:
                        raw, pending = pending.split(b'\n', 1)
                        line = raw.decode('utf-8', errors='replace').strip()
                        if line.startswith('AEC_'):
                            last_progress = time.monotonic()
                        if line.startswith(('AEC_', 'Guru Meditation', 'assert failed', 'abort()')):
                            print(line, flush=True)
                        if line.startswith('AEC_RESULT '):
                            summary['cases'].append(json.loads(line[len('AEC_RESULT '):]))
                        if line.startswith('AEC_MEMORY '):
                            summary['initialization_memory'].append(json.loads(line[len('AEC_MEMORY '):]))
                        if line.startswith('AEC_DONE '):
                            done = line == 'AEC_DONE PASS'
                            failure_reason = '' if done else 'fixture reported failure'
                if process.poll() is not None and not chunk:
                    failure_reason = 'emulator exited before AEC_DONE'
                    break
                if time.monotonic() - last_progress >= args.progress_timeout:
                    failure_reason = f'no fixture progress for {args.progress_timeout}s'
                    break
                time.sleep(0.1)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        emulator_log.close()
    expected = {'missing_psram'} if args.psram == 'none' else {'silence', 'far_only', 'near_only', 'doubletalk', 'path_change', 'clipping'}
    summary['passed'] = bool(done and {c['case'] for c in summary['cases']} == expected and
                             all(c['pass'] for c in summary['cases']))
    summary['failure_reason'] = failure_reason
    summary['source_matches_workspace'] = all(
        hashlib.sha256(sources[name].read_bytes()).hexdigest() == digest
        for name, digest in summary['source_sha256'].items())
    (artifact / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print('Result:', 'PASS' if summary['passed'] else 'FAIL (inspect qemu.log)', flush=True)
    if failure_reason:
        print('Reason:', failure_reason)
    print('The no-PSRAM case checks admission only; quality cases execute real DSP on synthetic PCM.')
    print('I2S, acoustic echo, Wi-Fi, and real-time deadlines require hardware.')
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
