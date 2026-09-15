# Convert MP3 to 16kHz Mono WAV using miniaudio (No ffmpeg needed!)
import os
import sys
import wave
import miniaudio
import numpy as np

def convert_mp3_to_wav(mp3_path, wav_path=None, target_sr=16000):
    if not os.path.exists(mp3_path):
        print(f"[ERROR] File not found: {mp3_path}")
        return False
        
    if wav_path is None:
        wav_path = os.path.splitext(mp3_path)[0] + ".wav"
        
    print(f"Decoding {mp3_path} using miniaudio...")
    decoded = miniaudio.decode_file(mp3_path)
    
    sr = decoded.sample_rate
    channels = decoded.nchannels
    raw_samples = np.array(decoded.samples, dtype=np.int16)
    
    print(f"Original: {sr} Hz, {channels} Channels, {len(raw_samples)} samples")
    
    # Convert stereo to mono
    if channels > 1:
        raw_samples = raw_samples.reshape(-1, channels).mean(axis=1).astype(np.int16)
        
    # Resample to target sample rate (default 16000 Hz for ESP32)
    if sr != target_sr:
        print(f"Resampling from {sr} Hz to {target_sr} Hz...")
        duration = len(raw_samples) / sr
        target_length = int(duration * target_sr)
        old_indices = np.linspace(0, len(raw_samples) - 1, len(raw_samples))
        new_indices = np.linspace(0, len(raw_samples) - 1, target_length)
        raw_samples = np.interp(new_indices, old_indices, raw_samples).astype(np.int16)
        sr = target_sr

    print(f"Writing 16kHz Mono WAV file: {wav_path}...")
    with wave.open(wav_path, 'wb') as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2) # 16-bit PCM
        wf.setframerate(sr)
        wf.writeframes(raw_samples.tobytes())
        
    print(f"[SUCCESS] Converted {mp3_path} -> {wav_path} ({len(raw_samples) / sr:.2f} seconds)")
    return True

if __name__ == "__main__":
    mp3_file = sys.argv[1] if len(sys.argv) > 1 else "song.mp3"
    convert_mp3_to_wav(mp3_file)
