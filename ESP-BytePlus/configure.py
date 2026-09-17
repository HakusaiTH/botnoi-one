"""Generate the ESP32's ignored configuration header from existing local settings."""

import argparse
import json
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def read_env(path):
    result = {}
    if not path.is_file():
        return result
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip().removeprefix("export ")
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = (part.strip() for part in line.split("=", 1))
        if key not in {"WIFI_SSID", "WIFI_PASS"} and not key.startswith(("BYTEPLUS_", "RTC_")):
            continue
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        else:
            value = value.split(" #", 1)[0].rstrip()
        result[key] = value
    return result


def settings():
    local = {}
    for path in (ROOT / "VoiceBot/.env", ROOT / "BytePlus/.env", ROOT / ".env"):
        local.update(read_env(path))
    reference = Path(os.environ.get("BYTEPLUS_ENV_FILE", local.get(
        "BYTEPLUS_ENV_FILE", "~/github/rtcbot/.env.local"))).expanduser()
    if ("BYTEPLUS_ENV_FILE" in os.environ or "BYTEPLUS_ENV_FILE" in local) and not reference.is_file():
        raise ValueError("BYTEPLUS_ENV_FILE does not exist")
    values = read_env(reference)
    values.update(local)
    values.update(os.environ)
    return values


def render(values, language, voice):
    required = ("WIFI_SSID", "BYTEPLUS_ASR_APP_ID", "BYTEPLUS_ASR_ACCESS_TOKEN",
                "BYTEPLUS_TTS_APP_ID", "BYTEPLUS_TTS_TOKEN", "BYTEPLUS_ARK_ENDPOINT_ID",
                "BYTEPLUS_ARK_API_KEY")
    missing = [key for key in required if not values.get(key, "").strip()]
    if missing:
        raise ValueError("Missing settings: " + ", ".join(missing))
    config = {key: values[key] for key in required}
    config["WIFI_PASS"] = values.get("WIFI_PASS", "")
    config["BYTEPLUS_LANGUAGE"] = language
    config["BYTEPLUS_TTS_VOICE"] = voice
    config["BYTEPLUS_TTS_RESOURCE_ID"] = values.get("BYTEPLUS_TTS_RESOURCE_ID", "").strip() or (
        "seed-tts-2.0" if voice.endswith("_uranus_bigtts") else "volc.service_type.1000009")
    lines = ["// Generated locally by configure.py. Do not commit this file.", "#pragma once"]
    for key, value in config.items():
        lines.append(f"#define {key} {json.dumps(value, ensure_ascii=False)}")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--language", choices=("en", "th"), default="th")
    parser.add_argument("--voice", help="An enabled BytePlus voice ID for the chosen language")
    args = parser.parse_args()
    try:
        values = settings()
        voice = args.voice or values.get("BYTEPLUS_TTS_VOICE_" + args.language.upper(), "").strip()
        if not voice:
            voice = ("th_female_bv568_neutral_uranus_bigtts" if args.language == "th"
                     else "en_female_anna_mars_bigtts")
        content = render(values, args.language, voice)
    except ValueError as exc:
        parser.error(str(exc))
    output = HERE / "config.local.h"
    # Open with private permissions from creation, including when replacing a file.
    descriptor = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as file:
            descriptor = None
            file.write(content)
    finally:
        if descriptor is not None:
            os.close(descriptor)
    print(f"Generated {output.relative_to(ROOT)} for {args.language}; credentials are not printed.")


if __name__ == "__main__":
    main()
