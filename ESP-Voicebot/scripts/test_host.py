#!/usr/bin/env python3
"""Run bounded transport, PCM and protocol host tests under ASan/UBSan."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    sketch = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arduinojson', type=Path, default=sketch / 'build/toolchain/user/libraries/ArduinoJson/src')
    args = parser.parse_args()
    if not (args.arduinojson / 'ArduinoJson.h').is_file():
        parser.error('Pass --arduinojson /path/to/ArduinoJson/src (ArduinoJson 7.4.3)')
    compiler = os.environ.get('CXX', 'c++')
    flags = ['-std=c++11', '-Wall', '-Wextra', '-Werror', '-g',
             '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    with tempfile.TemporaryDirectory(prefix='esp-voicebot-tests-') as tmp:
        for name in ('test_audio_pipeline', 'test_capture_pipeline', 'test_duplex_timeline', 'test_microphone_flow', 'test_playback_flow', 'test_session_flow', 'test_speaker_packetizer', 'test_websocket_stream', 'test_websocket_writable'):
            binary = Path(tmp) / name
            subprocess.run([compiler, *flags, str(sketch / 'tests' / (name + '.cpp')), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
        subprocess.run(['bash', str(sketch / 'tests/client/run.sh')], check=True,
                       env=dict(os.environ, ARDUINOJSON_DIR=str(args.arduinojson.resolve())))
        subprocess.run(['bash', str(sketch / 'tests/aec_wrapper/run.sh')], check=True)
    print('All host suites passed. Hardware I2S/Wi-Fi/TLS runtime still requires a board.')


if __name__ == '__main__':
    main()
