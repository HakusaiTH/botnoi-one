# Python UDP Audio Sender for ESP32 MAX98357A Test
#
# Supports .wav natively using Python wave module.
# Supports .mp3 using miniaudio (No ffmpeg required!).
#
# Usage:
#   python send_udp_audio.py <ESP32_IP> [AUDIO_FILE]
#
# Examples:
#   python send_udp_audio.py 10.25.157.95 song.wav
#   python send_udp_audio.py 10.25.157.95 song.mp3

import sys
import os
import socket
import time
import wave
import numpy as np

def load_audio_file(filename):
    ext = os.path.splitext(filename)[1].lower()
    
    if ext == ".wav":
        print(f"Opening WAV file natively: {filename}...")
        with wave.open(filename, 'rb') as wf:
            n_channels = wf.getnchannels()
            sample_width = wf.getsampwidth()
            framerate = wf.getframerate()
            n_frames = wf.getnframes()
            raw_data = wf.readframes(n_frames)
            
            if sample_width == 2:
                data = np.frombuffer(raw_data, dtype=np.int16)
            elif sample_width == 1:
                data = ((np.frombuffer(raw_data, dtype=np.uint8).astype(np.int32) - 128) * 256).astype(np.int16)
            elif sample_width == 4:
                data = (np.frombuffer(raw_data, dtype=np.int32) >> 16).astype(np.int16)
            else:
                raise ValueError(f"Unsupported sample width: {sample_width} bytes")

            if n_channels > 1:
                data = data.reshape(-1, n_channels).mean(axis=1).astype(np.int16)

            target_sr = 16000
            if framerate != target_sr:
                print(f"Resampling from {framerate} Hz to {target_sr} Hz...")
                duration = len(data) / framerate
                new_len = int(duration * target_sr)
                old_indices = np.linspace(0, len(data) - 1, len(data))
                new_indices = np.linspace(0, len(data) - 1, new_len)
                data = np.interp(new_indices, old_indices, data).astype(np.int16)
                
            return data
            
    else:
        # Use miniaudio to decode MP3 (no ffmpeg needed!)
        try:
            import miniaudio
            print(f"Decoding MP3 file natively using miniaudio: {filename}...")
            decoded = miniaudio.decode_file(filename)
            sr = decoded.sample_rate
            channels = decoded.nchannels
            raw_samples = np.array(decoded.samples, dtype=np.int16)
            
            if channels > 1:
                raw_samples = raw_samples.reshape(-1, channels).mean(axis=1).astype(np.int16)
                
            target_sr = 16000
            if sr != target_sr:
                print(f"Resampling from {sr} Hz to {target_sr} Hz...")
                duration = len(raw_samples) / sr
                new_len = int(duration * target_sr)
                old_indices = np.linspace(0, len(raw_samples) - 1, len(raw_samples))
                new_indices = np.linspace(0, len(raw_samples) - 1, new_len)
                raw_samples = np.interp(new_indices, old_indices, raw_samples).astype(np.int16)
                
            return raw_samples
        except Exception as e:
            print(f"\n[ERROR] Failed to decode audio file {filename}: {e}\n")
            raise e

def create_sample_wav_if_missing(filename="test_music.wav"):
    if os.path.exists(filename):
        return filename
    print(f"Generating sample test audio file: {filename}...")
    sample_rate = 16000
    duration = 5.0
    t = np.linspace(0, duration, int(sample_rate * duration), False)
    notes = [440.0, 554.37, 659.25, 880.0]
    audio = np.zeros_like(t)
    chunk_len = len(t) // len(notes)
    for i, note in enumerate(notes):
        sub_t = t[i*chunk_len : (i+1)*chunk_len]
        audio[i*chunk_len : (i+1)*chunk_len] = 0.5 * np.sin(2 * np.pi * note * sub_t)
        
    audio_int16 = (audio * 32767).astype(np.int16)
    with wave.open(filename, 'wb') as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(audio_int16.tobytes())
    return filename

def main():
    if len(sys.argv) < 2:
        print("Usage: python send_udp_audio.py <ESP32_IP> [AUDIO_FILE]")
        print("Example: python send_udp_audio.py 10.25.157.95 song.wav")
        sys.exit(1)

    udp_ip = sys.argv[1]
    udp_port = 9000
    chunk_size = 512
    
    filename = sys.argv[2] if len(sys.argv) >= 3 else (
        "song.wav" if os.path.exists("song.wav") else create_sample_wav_if_missing("test_music.wav")
    )
    
    try:
        audio_data = load_audio_file(filename)
    except Exception:
        sys.exit(1)

    print(f"Audio loaded successfully: {len(audio_data)} samples ({len(audio_data) / 16000:.2f} seconds)")

    scale_factor = 0.5
    audio_data = (audio_data * scale_factor).astype(np.int16)
    audio_data = np.clip(audio_data, -32768, 32767).astype(np.int16)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.05)

    print(f"Start streaming audio to ESP32 ({udp_ip}:{udp_port})...")
    idx = 0
    total_samples = len(audio_data)
    retry_count = 0

    while idx < total_samples:
        end_idx = idx + chunk_size
        chunk = audio_data[idx:end_idx]

        if len(chunk) < chunk_size:
            chunk = np.pad(chunk, (0, chunk_size - len(chunk)), 'constant')

        sock.sendto(chunk.tobytes(), (udp_ip, udp_port))

        try:
            data, _ = sock.recvfrom(16)
            if data == b'ACK':
                idx += chunk_size
                retry_count = 0
                time.sleep(0.004)
        except socket.timeout:
            retry_count += 1
            if retry_count > 100:
                print("Too many timeout retries. Stopping.")
                break
            time.sleep(0.01)

    print("\n[SUCCESS] Audio streaming finished!")
    sock.close()

if __name__ == "__main__":
    main()
