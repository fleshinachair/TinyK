import json
import struct
import sys

def parse_microkorg_syx(syx_path, output_json_path):
    with open(syx_path, 'rb') as f:
        data = f.read()

    # Locate sysex start (0xF0 0x42 ... 0xF7)
    # A full 128-program bank dump typically packs 128 programs into sequential frames
    # Each microKORG program dump header is: F0 42 30 58 40 (or 50)
    programs = []
    
    genre_names = [
        "Trance", "Techno/House", "Electronica", "DnB/Breaks",
        "Hiphop/Vintage", "Retro", "SE/Hit", "Vocoder"
    ]
    
    # Simple bank unpacker (assumes 128 dumps or single unpack payload)
    # Every program chunk contains the parameter byte block
    # Standard Korg MIDI spec offsets:
    # 0: Voice Mode, 1: Osc1 Wave, 2: Osc2 Wave, 3: Detune, 7: Cutoff, 8: Resonance, etc.
    
    # If using mK sound manager dumps:
    idx = 0
    prog_id = 0
    while idx < len(data) and prog_id < 128:
        # Find next Sysex message
        start = data.find(b'\xF0\x42', idx)
        if start == -1:
            break
        end = data.find(b'\xF7', start)
        if end == -1:
            break
            
        chunk = data[start:end+1]
        idx = end + 1
        
        # Check if valid microKORG program data (Function 0x58 or 0x40)
        if len(chunk) > 30 and chunk[3] == 0x58:
            bank = "A" if prog_id < 64 else "B"
            genre_idx = (prog_id % 64) // 8
            num = (prog_id % 8) + 1
            code = f"{bank}.{genre_idx+1}{num}"
            name = f"{code} {genre_names[genre_idx]}"

            # Parse parameters relative to program data payload
            # (Values normalized to 0.0 - 1.0 for Move)
            p = {
                "name": name,
                "cutoff": chunk[15] / 127.0 if len(chunk) > 15 else 0.7,
                "resonance": chunk[16] / 127.0 if len(chunk) > 16 else 0.2,
                "wave1": (chunk[12] & 0x07) / 7.0 if len(chunk) > 12 else 0.0,
                "wave2": (chunk[13] & 0x03) / 3.0 if len(chunk) > 13 else 0.0,
                "attack1": chunk[20] / 127.0 if len(chunk) > 20 else 0.01,
                "decay1": chunk[21] / 127.0 if len(chunk) > 21 else 0.5,
                "sustain1": chunk[22] / 127.0 if len(chunk) > 22 else 0.8,
                "release1": chunk[23] / 127.0 if len(chunk) > 23 else 0.2,
                "attack2": chunk[24] / 127.0 if len(chunk) > 24 else 0.01,
                "decay2": chunk[25] / 127.0 if len(chunk) > 25 else 0.5,
                "sustain2": chunk[26] / 127.0 if len(chunk) > 26 else 0.8,
                "release2": chunk[27] / 127.0 if len(chunk) > 27 else 0.2,
            }
            programs.append(p)
            prog_id += 1

    with open(output_json_path, 'w') as out:
        json.dump(programs, out, indent=2)
    print(f"Extracted {len(programs)} programs to {output_json_path}")

if __name__ == '__main__':
    syx_file = sys.argv[1] if len(sys.argv) > 1 else "tools/factory.syx"
    parse_microkorg_syx(syx_file, "presets.json")