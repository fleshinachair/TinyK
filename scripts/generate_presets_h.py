import json
import os

JSON_PATH = os.path.join("src", "presets.json")
with open(JSON_PATH, "r", encoding="utf-8") as f:
    presets = json.load(f)["presets"]

PARAM_FIELDS = [
    "wave1", "pulse_width", "wave2", "detune", "sync_ring", "osc_mix", "sub_level", "portamento",
    "cutoff", "resonance", "filter_type", "keytrack", "env_int", "drive",
    "attack1", "decay1", "sustain1", "release1",
    "attack2", "decay2", "sustain2", "release2",
    "chorus_mix", "delay_time", "delay_feedback", "delay_mix"
]

lines = []
lines.append("/* Auto-generated microKORG Factory Presets Header */")
lines.append("#ifndef PRESETS_H")
lines.append("#define PRESETS_H")
lines.append("")
lines.append("struct Preset {")
lines.append("    const char *label;")
for p in PARAM_FIELDS:
    lines.append(f"    float {p};")
lines.append("};")
lines.append("")
lines.append("static const struct Preset FACTORY_PRESETS[128] = {")

for idx, pr in enumerate(presets):
    label = pr.get("name", f"Preset {idx}")
    params = pr["params"]
    p_str = ", ".join(f"{params[p]:.4f}f" for p in PARAM_FIELDS)
    lines.append(f'    /* [{idx:3d}] {label} */')
    lines.append(f'    {{ "{label}",\n      {p_str} }}' + ("," if idx < 127 else ""))

lines.append("};")
lines.append("")
lines.append("#endif /* PRESETS_H */\n")

header_content = "\n".join(lines)

for target in [os.path.join("src", "dsp", "presets.h"), os.path.join("src", "presets.h")]:
    with open(target, "w", encoding="utf-8") as f:
        f.write(header_content)
    print(f"Generated {target} with {len(presets)} presets.")
