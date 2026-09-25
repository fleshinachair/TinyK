import json
import os

PARAM_NAMES = [
    "wave1", "pulse_width", "wave2", "detune", "sync_ring", "osc_mix", "sub_level", "portamento",
    "cutoff", "resonance", "filter_type", "keytrack", "env_int", "drive", "mod_int", "vel_sens",
    "attack1", "decay1", "sustain1", "release1", "attack2", "decay2", "sustain2", "release2",
    "lfo1_rate", "lfo2_rate", "chorus_mix", "delay_time", "delay_feedback", "delay_mix", "master_vol", "pan"
]

GENRES = [
    "1. Trance",
    "2. Techno/House",
    "3. Electronica",
    "4. DnB/Breaks",
    "5. Hiphop/Vintage",
    "6. Retro",
    "7. SE/Hit",
    "8. Vocoder"
]

GENRE_SHORT = [
    "Trance", "Techno/House", "Electronica", "DnB/Breaks",
    "Hiphop/Vintage", "Retro", "SE/Hit", "Vocoder"
]

# 128 Preset metadata: [Bank (A/B), GenreIdx (0..7), ProgNum (1..8), Name, Category, Description, ArchetypeKey]
PATCH_SPECS = [
    # --- Side A, Genre 1: Trance ---
    ("A", 0, 1, "Saw Lead", "Trance", "Classic supersaw trance lead with detune and stereo delay", "trance_lead"),
    ("A", 0, 2, "Trance Bass", "Trance", "Punchy 24dB LP offbeat rolling bass with sub", "trance_bass"),
    ("A", 0, 3, "Euro Pad", "Trance", "Lush detuned saw pad with rich chorus and slow release", "lush_pad"),
    ("A", 0, 4, "Gate Synth", "Trance", "Snappy rhythmic gated saw with ping-pong delay", "pluck"),
    ("A", 0, 5, "Hyper Saw", "Trance", "Intense bright dual saw lead with octave split", "trance_lead"),
    ("A", 0, 6, "Rave Pluck", "Trance", "Snappy percussive dance pluck with resonant filter decay", "pluck"),
    ("A", 0, 7, "Stadium Lead", "Trance", "Huge sync lead cutting through any mix with delay", "sync_lead"),
    ("A", 0, 8, "Anthem Pad", "Trance", "Grand uplifting polyphonic pad with warm 12dB filter", "lush_pad"),

    # --- Side A, Genre 2: Techno/House ---
    ("A", 1, 1, "Acid 303", "Techno/House", "Squelchy resonant lowpass bass with drive and envelope snap", "acid"),
    ("A", 1, 2, "Deep Organ", "Techno/House", "Classic 90s deep house organ with subtle chorus", "organ"),
    ("A", 1, 3, "Detroit Chord", "Techno/House", "Minor fifth detuned stab with snappy filter sweep", "chord_stab"),
    ("A", 1, 4, "Tech Stab", "Techno/House", "Punchy bandpass percussive chord stab", "chord_stab"),
    ("A", 1, 5, "Club Bass", "Techno/House", "Solid 4/4 driving low-end bass with sub-oscillator", "punchy_bass"),
    ("A", 1, 6, "Minimal Bleep", "Techno/House", "High-pitched resonant sequence bleep with echo", "bleep"),
    ("A", 1, 7, "Filter Clav", "Techno/House", "Funky rhythmic filtered square wave clavinet", "clav"),
    ("A", 1, 8, "Pump Lead", "Techno/House", "Compressed pumping dance lead with portamento", "sync_lead"),

    # --- Side A, Genre 3: Electronica ---
    ("A", 2, 1, "Ambient Drone", "Electronica", "Deep evolving continuous drone with LFO movement", "drone"),
    ("A", 2, 2, "Glitch Lead", "Electronica", "Ring-modulated sync lead with lo-fi edge", "ring_lead"),
    ("A", 2, 3, "Chill Pad", "Electronica", "Silky soft 12dB lowpass ambient wash with long reverb", "lush_pad"),
    ("A", 2, 4, "8-Bit Lo-Fi", "Electronica", "Chiptune-style raw pulse wave sound", "chiptune"),
    ("A", 2, 5, "Crystal Bell", "Electronica", "Bright sparkling bell tone with wide stereo chorus", "bell"),
    ("A", 2, 6, "Sub Sine Bass", "Electronica", "Pure deep sub-bass with gentle saturation", "sub_bass"),
    ("A", 2, 7, "Space Texture", "Electronica", "Resonant bandpass sweep drifting across delay taps", "texture"),
    ("A", 2, 8, "Modular Sequence", "Electronica", "Snappy modular synthesizer sequence with fast filter env", "pluck"),

    # --- Side A, Genre 4: DnB/Breaks ---
    ("A", 3, 1, "Reese Bass", "DnB/Breaks", "Heavy detuned dual saw bass with phase beating and drive", "reese"),
    ("A", 3, 2, "Jungle Sub", "DnB/Breaks", "Deep 808-style sustained sub bass with punchy transient", "sub_bass"),
    ("A", 3, 3, "Siren Lead", "DnB/Breaks", "Piercing high-pitch modulation siren synth", "sync_lead"),
    ("A", 3, 4, "Break Pluck", "DnB/Breaks", "Fast percussive synth pluck for intricate breakbeats", "pluck"),
    ("A", 3, 5, "Neuro Bass", "DnB/Breaks", "Distorted bandpass bass with aggressive drive and resonance", "acid"),
    ("A", 3, 6, "Liquid Pad", "DnB/Breaks", "Smooth floating liquid drum & bass pad with chorus", "lush_pad"),
    ("A", 3, 7, "2-Step Bass", "DnB/Breaks", "Warm bouncy UK garage bass with snappy attack", "punchy_bass"),
    ("A", 3, 8, "Rave Stab", "DnB/Breaks", "Classic early 90s jungle/hardcore rave chord hit", "chord_stab"),

    # --- Side A, Genre 5: Hiphop/Vintage ---
    ("A", 4, 1, "Funk Moog", "Hiphop/Vintage", "Classic fat analog ladder-style lowpass bass", "punchy_bass"),
    ("A", 4, 2, "G-Funk Lead", "Hiphop/Vintage", "Smooth gliding sine/triangle whistle lead with portamento", "whistle"),
    ("A", 4, 3, "Vintage EP", "Hiphop/Vintage", "Electric piano style warm keys with chorus", "ep"),
    ("A", 4, 4, "West Coast Whistle", "Hiphop/Vintage", "High soaring portamento sine lead", "whistle"),
    ("A", 4, 5, "Motown Low", "Hiphop/Vintage", "Deep rounded vintage bass tone", "sub_bass"),
    ("A", 4, 6, "Oldschool Brass", "Hiphop/Vintage", "Warm analog polyphonic synth brass swell", "brass"),
    ("A", 4, 7, "P-Funk Lead", "Hiphop/Vintage", "Hard sync funk solo synth with pitch envelope", "sync_lead"),
    ("A", 4, 8, "Retro Flute", "Hiphop/Vintage", "Airy triangle/sine flute with subtle vibrato", "flute"),

    # --- Side A, Genre 6: Retro ---
    ("A", 5, 1, "1984 Brass", "Retro", "Iconic 80s analog synth brass with rich unison detune", "brass"),
    ("A", 5, 2, "Synthwave Arp", "Retro", "Bright punchy 16th-note running bassline", "punchy_bass"),
    ("A", 5, 3, "Stranger Pad", "Retro", "Nostalgic warm analog pad with chorus", "lush_pad"),
    ("A", 5, 4, "Poly 800 Strings", "Retro", "Vintage thin ensemble strings with high resonance", "strings"),
    ("A", 5, 5, "Miami Bass", "Retro", "Snappy electro 808 boogie bass", "punchy_bass"),
    ("A", 5, 6, "Blade Lead", "Retro", "Cinematic CS-80 style expressive lead with slow glide", "cinematic_lead"),
    ("A", 5, 7, "New Wave Pluck", "Retro", "Chorusy guitar-synth pluck from the early 80s", "pluck"),
    ("A", 5, 8, "Disco Octave", "Retro", "Driving disco octave square bass", "punchy_bass"),

    # --- Side A, Genre 7: SE/Hit ---
    ("A", 6, 1, "Laser Sweep", "SE/Hit", "Downward pitch envelope resonant laser zap", "sfx_laser"),
    ("A", 6, 2, "Thunder Drop", "SE/Hit", "Massive low-end impact with long delay trail", "sfx_drop"),
    ("A", 6, 3, "Metallic Zap", "SE/Hit", "Ring-modulated percussive metallic impact", "sfx_zap"),
    ("A", 6, 4, "Ring Drone", "SE/Hit", "Dissonant sci-fi ring modulation atmospheric drone", "drone"),
    ("A", 6, 5, "Psycho Bell", "SE/Hit", "Eerie detuned dissonant horror chime", "bell"),
    ("A", 6, 6, "Noise Sweep", "SE/Hit", "Resonant filter sweep noise buildup effect", "sfx_sweep"),
    ("A", 6, 7, "Alien UFO", "SE/Hit", "Fast LFO modulated extraterrestrial wobble", "texture"),
    ("A", 6, 8, "Space Beacon", "SE/Hit", "Echoing sonar pulse drifting into deep delay", "bleep"),

    # --- Side A, Genre 8: Vocoder ---
    ("A", 7, 1, "Robot Vox", "Vocoder", "Classic robotic formant-filtered synth sound", "vocoder_formant"),
    ("A", 7, 2, "Choir Vox", "Vocoder", "Harmonic vowel choir ensemble with chorus", "vocoder_choir"),
    ("A", 7, 3, "Talkbox Synth", "Vocoder", "Funky vowel-modulated talkbox lead", "vocoder_talkbox"),
    ("A", 7, 4, "Vocoder Bass", "Vocoder", "Buzzing vocal formant bass with punch", "vocoder_bass"),
    ("A", 7, 5, "Whispering Vox", "Vocoder", "Airy highpass formant vocal texture", "vocoder_whisper"),
    ("A", 7, 6, "Android Pad", "Vocoder", "Lush cybernetic choral pad with slow attack", "vocoder_choir"),
    ("A", 7, 7, "Cyborg Solo", "Vocoder", "Cutting expressive vocal solo synth", "vocoder_talkbox"),
    ("A", 7, 8, "Vocoder Sweep", "Vocoder", "Resonant formant filter sweep with delay", "vocoder_formant"),

    # --- Side B, Genre 1: Trance ---
    ("B", 0, 1, "Goa Trance", "Trance", "Acidic high-resonance psychedelic lead with pitch glide", "acid"),
    ("B", 0, 2, "Hardstyle Bass", "Trance", "Distorted punchy offbeat bass with drive saturation", "punchy_bass"),
    ("B", 0, 3, "Uplifting Strings", "Trance", "Massive euphoric trance string section with chorus", "strings"),
    ("B", 0, 4, "Psytrance Arp", "Trance", "Tight percussive 16th-note pluck with snappy envelope", "pluck"),
    ("B", 0, 5, "Trancegate Pad", "Trance", "Rhythmic chopped chords with delay and bright cutoff", "trance_lead"),
    ("B", 0, 6, "Energy Pluck", "Trance", "Dual saw bright pluck with ping-pong delay feedback", "pluck"),
    ("B", 0, 7, "Hard Trance Lead", "Trance", "Aggressive hard-sync lead with biting drive", "sync_lead"),
    ("B", 0, 8, "Dreamscape", "Trance", "Ethereal floating ambient trance soundscape", "lush_pad"),

    # --- Side B, Genre 2: Techno/House ---
    ("B", 1, 1, "Chicago Bass", "Techno/House", "Raw analog square bass from the Chicago underground", "punchy_bass"),
    ("B", 1, 2, "Deep Tech Chord", "Techno/House", "Dub techno minor chord with long echoing delay", "chord_stab"),
    ("B", 1, 3, "French Filter", "Techno/House", "Filtered disco saw sweep with high resonance", "trance_lead"),
    ("B", 1, 4, "Techno Drone", "Techno/House", "Dark industrial resonant rumble with sub-oscillator", "drone"),
    ("B", 1, 5, "Garage Bass", "Techno/House", "Deep 2-step garage bass with warm overdrive", "punchy_bass"),
    ("B", 1, 6, "Sub Drop", "Techno/House", "Pitch-sliding low frequency boom", "sub_bass"),
    ("B", 1, 7, "Electro Clash", "Techno/House", "Gritty distorted analog saw with snappy attack", "acid"),
    ("B", 1, 8, "Jacking Pluck", "Techno/House", "Hollow punchy house pluck with chorus", "pluck"),

    # --- Side B, Genre 3: Electronica ---
    ("B", 2, 1, "Glitch Pad", "Electronica", "Sample & hold modulated randomized ambient pad", "texture"),
    ("B", 2, 2, "Metallic Bell", "Electronica", "Ring modulated resonant bell tone", "bell"),
    ("B", 2, 3, "Warm Glass", "Electronica", "Fragile crystalline glass pad with chorus", "lush_pad"),
    ("B", 2, 4, "Granular Tone", "Electronica", "Shimmering high frequency modulated timbre", "texture"),
    ("B", 2, 5, "Dark Drone", "Electronica", "Ominous cinematic low drone with slow movement", "drone"),
    ("B", 2, 6, "Modular Bass", "Electronica", "FM-like metallic bass with snappy envelope", "acid"),
    ("B", 2, 7, "Shimmer Pad", "Electronica", "High octaves detuned pad with sparkling release", "lush_pad"),
    ("B", 2, 8, "Space Echoes", "Electronica", "Self-oscillating delay texture with subtle chorus", "bleep"),

    # --- Side B, Genre 4: DnB/Breaks ---
    ("B", 3, 1, "Darkstep Reese", "DnB/Breaks", "Menacing distorted reese bass with heavy sub", "reese"),
    ("B", 3, 2, "808 Sub Boom", "DnB/Breaks", "Ultra-clean sustained low sine sub-bass", "sub_bass"),
    ("B", 3, 3, "Hardstep Horn", "DnB/Breaks", "Brassy aggressive synthesizer horn stab", "brass"),
    ("B", 3, 4, "Rollers Bass", "DnB/Breaks", "Warm modulated rolling bass with sub octave", "punchy_bass"),
    ("B", 3, 5, "Jungle Flute", "DnB/Breaks", "Airy high-register melodic flute for jungle leads", "flute"),
    ("B", 3, 6, "Amen Stab", "DnB/Breaks", "Heavily driven punchy chord hit", "chord_stab"),
    ("B", 3, 7, "Screamer Lead", "DnB/Breaks", "High resonance bandpass screaming synth lead", "sync_lead"),
    ("B", 3, 8, "Atmospheric DnB", "DnB/Breaks", "Deep floating dream pad with long delay", "lush_pad"),

    # --- Side B, Genre 5: Hiphop/Vintage ---
    ("B", 4, 1, "Talkin Bass", "Hiphop/Vintage", "Vocal formant filtered funk bassline", "vocoder_bass"),
    ("B", 4, 2, "Vintage Poly EP", "Hiphop/Vintage", "Mellow electric piano with warm vibrato", "ep"),
    ("B", 4, 3, "Warm 5ths", "Hiphop/Vintage", "Fifth-interval detuned jazz-funk poly keys", "chord_stab"),
    ("B", 4, 4, "R&B Sine", "Hiphop/Vintage", "Silky smooth modern R&B gliding sine lead", "whistle"),
    ("B", 4, 5, "70s Solina", "Hiphop/Vintage", "Authentic 70s ensemble string machine simulation", "strings"),
    ("B", 4, 6, "P-Funk Clav", "Hiphop/Vintage", "Biting vintage clavinet with drive and keytrack", "clav"),
    ("B", 4, 7, "Classic Whistle", "Hiphop/Vintage", "Iconic Dr. Dre style high sine whistle", "whistle"),
    ("B", 4, 8, "Retro Organ", "Hiphop/Vintage", "Warm tonewheel combo organ with subtle vibrato", "organ"),

    # --- Side B, Genre 6: Retro ---
    ("B", 5, 1, "Synthpop Lead", "Retro", "Punchy 80s new wave lead with subtle chorus", "trance_lead"),
    ("B", 5, 2, "Vaporwave Pad", "Retro", "Detuned lo-fi tape pad with slow gentle wow/flutter", "lush_pad"),
    ("B", 5, 3, "Italo Disco Bass", "Retro", "Bouncing 16th-note analog bass with fast decay", "punchy_bass"),
    ("B", 5, 4, "Stranger Bells", "Retro", "Nostalgic arpeggio synth chime from the 80s", "bell"),
    ("B", 5, 5, "VHS Strings", "Retro", "Warm analog strings washed in stereo chorus", "strings"),
    ("B", 5, 6, "Cyberpunk Saw", "Retro", "Aggressive modern retro saw lead with drive", "trance_lead"),
    ("B", 5, 7, "Analog Brass II", "Retro", "Slow blooming brass swell with rich filter resonance", "brass"),
    ("B", 5, 8, "Retrowave Arp", "Retro", "Classic outrun running synth pluck with delay", "pluck"),

    # --- Side B, Genre 7: SE/Hit ---
    ("B", 6, 1, "Alien Radio", "SE/Hit", "Random sample-and-hold modulated interstellar noise", "texture"),
    ("B", 6, 2, "Hydro Sweep", "SE/Hit", "Liquid resonant filter sweep across high resonance", "sfx_sweep"),
    ("B", 6, 3, "Cosmic Ray", "SE/Hit", "Ultra-fast pitch modulation laser raygun", "sfx_laser"),
    ("B", 6, 4, "Industrial Clang", "SE/Hit", "Metallic ring-modulated percussion hit", "sfx_zap"),
    ("B", 6, 5, "Warp Drive", "SE/Hit", "Rising pitch acceleration engine effect", "sfx_laser"),
    ("B", 6, 6, "Seismic Hit", "SE/Hit", "Sub-heavy impact hit with stereo echo", "sfx_drop"),
    ("B", 6, 7, "Cyber Glitch", "SE/Hit", "High speed modulated digital glitch hit", "bleep"),
    ("B", 6, 8, "Vortex Beam", "SE/Hit", "Swirling stereo noise beam with delay", "texture"),

    # --- Side B, Genre 8: Vocoder ---
    ("B", 7, 1, "Vocoder Ensemble", "Vocoder", "Rich choral vocoder backing ensemble", "vocoder_choir"),
    ("B", 7, 2, "Android Speech", "Vocoder", "Crisp mechanical robotic speech formant sound", "vocoder_formant"),
    ("B", 7, 3, "Vocoder Strings", "Vocoder", "Vocal-filtered string ensemble with chorus", "vocoder_choir"),
    ("B", 7, 4, "Funky Robot", "Vocoder", "Short snappy vocoder funk rhythm stab", "vocoder_talkbox"),
    ("B", 7, 5, "Space Vocoder", "Vocoder", "Reverberant deep space robotic choir", "vocoder_choir"),
    ("B", 7, 6, "Digital Voweller", "Vocoder", "Modulating formant vowel synthesizer", "vocoder_talkbox"),
    ("B", 7, 7, "Cyber Choir", "Vocoder", "Haunting ethereal synthesized choir pad", "vocoder_choir"),
    ("B", 7, 8, "Reso Formant Drone","Vocoder", "Continuous resonant vocoder formant drone", "vocoder_formant"),
]

# Base Archetypes for Parameter Generation
ARCHETYPES = {
    "trance_lead": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.0, "detune": 0.54, "sync_ring": 0.0,
        "osc_mix": 0.5, "sub_level": 0.1, "portamento": 0.05,
        "cutoff": 0.75, "resonance": 0.35, "filter_type": 0.0, "keytrack": 0.6,
        "env_int": 0.65, "drive": 0.25, "mod_int": 0.15, "vel_sens": 0.3,
        "attack1": 0.01, "decay1": 0.45, "sustain1": 0.6, "release1": 0.3,
        "attack2": 0.01, "decay2": 0.5, "sustain2": 0.85, "release2": 0.25,
        "lfo1_rate": 0.35, "lfo2_rate": 0.4, "chorus_mix": 0.35,
        "delay_time": 0.375, "delay_feedback": 0.4, "delay_mix": 0.3,
        "master_vol": 0.82, "pan": 0.5
    },
    "trance_bass": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.33, "detune": 0.5, "sync_ring": 0.0,
        "osc_mix": 0.3, "sub_level": 0.75, "portamento": 0.02,
        "cutoff": 0.28, "resonance": 0.38, "filter_type": 0.0, "keytrack": 0.5,
        "env_int": 0.75, "drive": 0.35, "mod_int": 0.0, "vel_sens": 0.35,
        "attack1": 0.01, "decay1": 0.3, "sustain1": 0.1, "release1": 0.15,
        "attack2": 0.01, "decay2": 0.35, "sustain2": 0.6, "release2": 0.15,
        "lfo1_rate": 0.2, "lfo2_rate": 0.3, "chorus_mix": 0.0,
        "delay_time": 0.25, "delay_feedback": 0.0, "delay_mix": 0.0,
        "master_vol": 0.85, "pan": 0.5
    },
    "punchy_bass": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.5, "detune": 0.5, "sync_ring": 0.0,
        "osc_mix": 0.4, "sub_level": 0.8, "portamento": 0.0,
        "cutoff": 0.22, "resonance": 0.45, "filter_type": 0.0, "keytrack": 0.45,
        "env_int": 0.78, "drive": 0.4, "mod_int": 0.0, "vel_sens": 0.4,
        "attack1": 0.008, "decay1": 0.28, "sustain1": 0.05, "release1": 0.12,
        "attack2": 0.008, "decay2": 0.32, "sustain2": 0.5, "release2": 0.12,
        "lfo1_rate": 0.2, "lfo2_rate": 0.2, "chorus_mix": 0.0,
        "delay_time": 0.2, "delay_feedback": 0.0, "delay_mix": 0.0,
        "master_vol": 0.88, "pan": 0.5
    },
    "sub_bass": {
        "wave1": 1.0, "pulse_width": 0.5, "wave2": 1.0, "detune": 0.5, "sync_ring": 0.0,
        "osc_mix": 0.2, "sub_level": 0.9, "portamento": 0.05,
        "cutoff": 0.2, "resonance": 0.15, "filter_type": 0.0, "keytrack": 0.5,
        "env_int": 0.5, "drive": 0.2, "mod_int": 0.0, "vel_sens": 0.3,
        "attack1": 0.01, "decay1": 0.5, "sustain1": 0.9, "release1": 0.2,
        "attack2": 0.01, "decay2": 0.5, "sustain2": 0.95, "release2": 0.2,
        "lfo1_rate": 0.1, "lfo2_rate": 0.1, "chorus_mix": 0.0,
        "delay_time": 0.2, "delay_feedback": 0.0, "delay_mix": 0.0,
        "master_vol": 0.9, "pan": 0.5
    },
    "reese": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.0, "detune": 0.535, "sync_ring": 0.0,
        "osc_mix": 0.5, "sub_level": 0.6, "portamento": 0.1,
        "cutoff": 0.35, "resonance": 0.3, "filter_type": 0.33, "keytrack": 0.4,
        "env_int": 0.55, "drive": 0.5, "mod_int": 0.0, "vel_sens": 0.2,
        "attack1": 0.02, "decay1": 0.6, "sustain1": 0.85, "release1": 0.3,
        "attack2": 0.02, "decay2": 0.6, "sustain2": 0.9, "release2": 0.3,
        "lfo1_rate": 0.2, "lfo2_rate": 0.15, "chorus_mix": 0.4,
        "delay_time": 0.25, "delay_feedback": 0.1, "delay_mix": 0.05,
        "master_vol": 0.85, "pan": 0.5
    },
    "acid": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.5, "detune": 0.5, "sync_ring": 0.0,
        "osc_mix": 0.1, "sub_level": 0.2, "portamento": 0.12,
        "cutoff": 0.3, "resonance": 0.82, "filter_type": 0.0, "keytrack": 0.65,
        "env_int": 0.88, "drive": 0.55, "mod_int": 0.0, "vel_sens": 0.5,
        "attack1": 0.005, "decay1": 0.25, "sustain1": 0.05, "release1": 0.1,
        "attack2": 0.005, "decay2": 0.3, "sustain2": 0.4, "release2": 0.1,
        "lfo1_rate": 0.4, "lfo2_rate": 0.3, "chorus_mix": 0.0,
        "delay_time": 0.33, "delay_feedback": 0.35, "delay_mix": 0.25,
        "master_vol": 0.8, "pan": 0.5
    },
    "pluck": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.5, "detune": 0.52, "sync_ring": 0.0,
        "osc_mix": 0.4, "sub_level": 0.15, "portamento": 0.0,
        "cutoff": 0.4, "resonance": 0.45, "filter_type": 0.0, "keytrack": 0.7,
        "env_int": 0.78, "drive": 0.2, "mod_int": 0.0, "vel_sens": 0.5,
        "attack1": 0.005, "decay1": 0.25, "sustain1": 0.1, "release1": 0.15,
        "attack2": 0.005, "decay2": 0.3, "sustain2": 0.15, "release2": 0.15,
        "lfo1_rate": 0.5, "lfo2_rate": 0.4, "chorus_mix": 0.25,
        "delay_time": 0.375, "delay_feedback": 0.5, "delay_mix": 0.35,
        "master_vol": 0.82, "pan": 0.5
    },
    "lush_pad": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.0, "detune": 0.54, "sync_ring": 0.0,
        "osc_mix": 0.5, "sub_level": 0.2, "portamento": 0.0,
        "cutoff": 0.42, "resonance": 0.25, "filter_type": 0.33, "keytrack": 0.5,
        "env_int": 0.6, "drive": 0.1, "mod_int": 0.25, "vel_sens": 0.25,
        "attack1": 0.4, "decay1": 0.6, "sustain1": 0.7, "release1": 0.6,
        "attack2": 0.35, "decay2": 0.6, "sustain2": 0.85, "release2": 0.65,
        "lfo1_rate": 0.18, "lfo2_rate": 0.12, "chorus_mix": 0.6,
        "delay_time": 0.5, "delay_feedback": 0.5, "delay_mix": 0.35,
        "master_vol": 0.78, "pan": 0.5
    },
    "strings": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.0, "detune": 0.53, "sync_ring": 0.0,
        "osc_mix": 0.5, "sub_level": 0.1, "portamento": 0.02,
        "cutoff": 0.55, "resonance": 0.3, "filter_type": 0.33, "keytrack": 0.6,
        "env_int": 0.55, "drive": 0.15, "mod_int": 0.2, "vel_sens": 0.3,
        "attack1": 0.25, "decay1": 0.5, "sustain1": 0.8, "release1": 0.5,
        "attack2": 0.2, "decay2": 0.5, "sustain2": 0.85, "release2": 0.55,
        "lfo1_rate": 0.25, "lfo2_rate": 0.2, "chorus_mix": 0.65,
        "delay_time": 0.4, "delay_feedback": 0.35, "delay_mix": 0.25,
        "master_vol": 0.8, "pan": 0.5
    },
    "brass": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.0, "detune": 0.535, "sync_ring": 0.0,
        "osc_mix": 0.5, "sub_level": 0.1, "portamento": 0.0,
        "cutoff": 0.35, "resonance": 0.4, "filter_type": 0.0, "keytrack": 0.6,
        "env_int": 0.75, "drive": 0.25, "mod_int": 0.1, "vel_sens": 0.45,
        "attack1": 0.08, "decay1": 0.45, "sustain1": 0.65, "release1": 0.3,
        "attack2": 0.06, "decay2": 0.5, "sustain2": 0.85, "release2": 0.3,
        "lfo1_rate": 0.3, "lfo2_rate": 0.25, "chorus_mix": 0.35,
        "delay_time": 0.3, "delay_feedback": 0.25, "delay_mix": 0.2,
        "master_vol": 0.82, "pan": 0.5
    },
    "sync_lead": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.0, "detune": 0.54, "sync_ring": 0.33,
        "osc_mix": 0.6, "sub_level": 0.1, "portamento": 0.15,
        "cutoff": 0.68, "resonance": 0.5, "filter_type": 0.0, "keytrack": 0.75,
        "env_int": 0.7, "drive": 0.4, "mod_int": 0.25, "vel_sens": 0.3,
        "attack1": 0.015, "decay1": 0.4, "sustain1": 0.65, "release1": 0.3,
        "attack2": 0.01, "decay2": 0.4, "sustain2": 0.9, "release2": 0.25,
        "lfo1_rate": 0.4, "lfo2_rate": 0.45, "chorus_mix": 0.3,
        "delay_time": 0.375, "delay_feedback": 0.45, "delay_mix": 0.35,
        "master_vol": 0.8, "pan": 0.5
    },
    "chord_stab": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.0, "detune": 0.646, "sync_ring": 0.0,
        "osc_mix": 0.5, "sub_level": 0.2, "portamento": 0.0,
        "cutoff": 0.45, "resonance": 0.5, "filter_type": 0.0, "keytrack": 0.7,
        "env_int": 0.72, "drive": 0.3, "mod_int": 0.0, "vel_sens": 0.4,
        "attack1": 0.01, "decay1": 0.3, "sustain1": 0.2, "release1": 0.2,
        "attack2": 0.008, "decay2": 0.35, "sustain2": 0.3, "release2": 0.2,
        "lfo1_rate": 0.3, "lfo2_rate": 0.3, "chorus_mix": 0.2,
        "delay_time": 0.375, "delay_feedback": 0.5, "delay_mix": 0.4,
        "master_vol": 0.82, "pan": 0.5
    },
    "organ": {
        "wave1": 1.0, "pulse_width": 0.5, "wave2": 1.0, "detune": 0.75, "sync_ring": 0.0,
        "osc_mix": 0.5, "sub_level": 0.4, "portamento": 0.0,
        "cutoff": 0.7, "resonance": 0.1, "filter_type": 0.33, "keytrack": 0.5,
        "env_int": 0.5, "drive": 0.25, "mod_int": 0.1, "vel_sens": 0.2,
        "attack1": 0.005, "decay1": 0.5, "sustain1": 1.0, "release1": 0.08,
        "attack2": 0.005, "decay2": 0.5, "sustain2": 1.0, "release2": 0.08,
        "lfo1_rate": 0.5, "lfo2_rate": 0.5, "chorus_mix": 0.5,
        "delay_time": 0.2, "delay_feedback": 0.1, "delay_mix": 0.1,
        "master_vol": 0.82, "pan": 0.5
    },
    "ep": {
        "wave1": 1.0, "pulse_width": 0.5, "wave2": 0.66, "detune": 0.505, "sync_ring": 0.0,
        "osc_mix": 0.4, "sub_level": 0.1, "portamento": 0.0,
        "cutoff": 0.5, "resonance": 0.2, "filter_type": 0.0, "keytrack": 0.6,
        "env_int": 0.65, "drive": 0.15, "mod_int": 0.1, "vel_sens": 0.6,
        "attack1": 0.01, "decay1": 0.45, "sustain1": 0.2, "release1": 0.25,
        "attack2": 0.008, "decay2": 0.5, "sustain2": 0.4, "release2": 0.25,
        "lfo1_rate": 0.3, "lfo2_rate": 0.2, "chorus_mix": 0.45,
        "delay_time": 0.3, "delay_feedback": 0.2, "delay_mix": 0.15,
        "master_vol": 0.82, "pan": 0.5
    },
    "clav": {
        "wave1": 0.33, "pulse_width": 0.25, "wave2": 0.5, "detune": 0.5, "sync_ring": 0.0,
        "osc_mix": 0.4, "sub_level": 0.0, "portamento": 0.0,
        "cutoff": 0.45, "resonance": 0.6, "filter_type": 0.66, "keytrack": 0.8,
        "env_int": 0.82, "drive": 0.3, "mod_int": 0.0, "vel_sens": 0.6,
        "attack1": 0.005, "decay1": 0.22, "sustain1": 0.05, "release1": 0.1,
        "attack2": 0.005, "decay2": 0.25, "sustain2": 0.2, "release2": 0.1,
        "lfo1_rate": 0.4, "lfo2_rate": 0.4, "chorus_mix": 0.1,
        "delay_time": 0.2, "delay_feedback": 0.1, "delay_mix": 0.1,
        "master_vol": 0.85, "pan": 0.5
    },
    "bell": {
        "wave1": 1.0, "pulse_width": 0.5, "wave2": 0.66, "detune": 0.75, "sync_ring": 0.66,
        "osc_mix": 0.6, "sub_level": 0.0, "portamento": 0.0,
        "cutoff": 0.7, "resonance": 0.4, "filter_type": 0.0, "keytrack": 0.7,
        "env_int": 0.6, "drive": 0.1, "mod_int": 0.1, "vel_sens": 0.5,
        "attack1": 0.005, "decay1": 0.5, "sustain1": 0.1, "release1": 0.4,
        "attack2": 0.005, "decay2": 0.6, "sustain2": 0.2, "release2": 0.45,
        "lfo1_rate": 0.3, "lfo2_rate": 0.2, "chorus_mix": 0.5,
        "delay_time": 0.4, "delay_feedback": 0.5, "delay_mix": 0.35,
        "master_vol": 0.8, "pan": 0.5
    },
    "whistle": {
        "wave1": 1.0, "pulse_width": 0.5, "wave2": 0.66, "detune": 0.5, "sync_ring": 0.0,
        "osc_mix": 0.15, "sub_level": 0.0, "portamento": 0.28,
        "cutoff": 0.85, "resonance": 0.1, "filter_type": 0.0, "keytrack": 0.8,
        "env_int": 0.5, "drive": 0.05, "mod_int": 0.3, "vel_sens": 0.3,
        "attack1": 0.05, "decay1": 0.4, "sustain1": 0.9, "release1": 0.2,
        "attack2": 0.04, "decay2": 0.4, "sustain2": 0.9, "release2": 0.2,
        "lfo1_rate": 0.35, "lfo2_rate": 0.2, "chorus_mix": 0.2,
        "delay_time": 0.35, "delay_feedback": 0.4, "delay_mix": 0.25,
        "master_vol": 0.85, "pan": 0.5
    },
    "flute": {
        "wave1": 0.66, "pulse_width": 0.5, "wave2": 1.0, "detune": 0.5, "sync_ring": 0.0,
        "osc_mix": 0.3, "sub_level": 0.0, "portamento": 0.1,
        "cutoff": 0.6, "resonance": 0.2, "filter_type": 0.33, "keytrack": 0.7,
        "env_int": 0.55, "drive": 0.1, "mod_int": 0.25, "vel_sens": 0.4,
        "attack1": 0.08, "decay1": 0.4, "sustain1": 0.85, "release1": 0.25,
        "attack2": 0.06, "decay2": 0.4, "sustain2": 0.9, "release2": 0.25,
        "lfo1_rate": 0.35, "lfo2_rate": 0.2, "chorus_mix": 0.3,
        "delay_time": 0.375, "delay_feedback": 0.35, "delay_mix": 0.25,
        "master_vol": 0.82, "pan": 0.5
    },
    "cinematic_lead": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.0, "detune": 0.525, "sync_ring": 0.0,
        "osc_mix": 0.5, "sub_level": 0.15, "portamento": 0.2,
        "cutoff": 0.55, "resonance": 0.45, "filter_type": 0.33, "keytrack": 0.65,
        "env_int": 0.65, "drive": 0.25, "mod_int": 0.35, "vel_sens": 0.4,
        "attack1": 0.15, "decay1": 0.5, "sustain1": 0.8, "release1": 0.45,
        "attack2": 0.1, "decay2": 0.5, "sustain2": 0.9, "release2": 0.45,
        "lfo1_rate": 0.28, "lfo2_rate": 0.2, "chorus_mix": 0.5,
        "delay_time": 0.45, "delay_feedback": 0.5, "delay_mix": 0.35,
        "master_vol": 0.8, "pan": 0.5
    },
    "chiptune": {
        "wave1": 0.33, "pulse_width": 0.15, "wave2": 0.5, "detune": 0.5, "sync_ring": 0.0,
        "osc_mix": 0.0, "sub_level": 0.0, "portamento": 0.0,
        "cutoff": 0.95, "resonance": 0.1, "filter_type": 0.0, "keytrack": 0.5,
        "env_int": 0.5, "drive": 0.1, "mod_int": 0.0, "vel_sens": 0.1,
        "attack1": 0.001, "decay1": 0.2, "sustain1": 0.8, "release1": 0.05,
        "attack2": 0.001, "decay2": 0.2, "sustain2": 0.8, "release2": 0.05,
        "lfo1_rate": 0.5, "lfo2_rate": 0.5, "chorus_mix": 0.0,
        "delay_time": 0.15, "delay_feedback": 0.2, "delay_mix": 0.1,
        "master_vol": 0.82, "pan": 0.5
    },
    "bleep": {
        "wave1": 1.0, "pulse_width": 0.5, "wave2": 0.66, "detune": 0.75, "sync_ring": 0.0,
        "osc_mix": 0.5, "sub_level": 0.0, "portamento": 0.0,
        "cutoff": 0.65, "resonance": 0.7, "filter_type": 0.66, "keytrack": 0.8,
        "env_int": 0.75, "drive": 0.2, "mod_int": 0.0, "vel_sens": 0.4,
        "attack1": 0.005, "decay1": 0.18, "sustain1": 0.05, "release1": 0.1,
        "attack2": 0.005, "decay2": 0.2, "sustain2": 0.1, "release2": 0.1,
        "lfo1_rate": 0.6, "lfo2_rate": 0.4, "chorus_mix": 0.15,
        "delay_time": 0.375, "delay_feedback": 0.6, "delay_mix": 0.45,
        "master_vol": 0.8, "pan": 0.5
    },
    "drone": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.33, "detune": 0.52, "sync_ring": 0.0,
        "osc_mix": 0.5, "sub_level": 0.6, "portamento": 0.3,
        "cutoff": 0.35, "resonance": 0.5, "filter_type": 0.33, "keytrack": 0.2,
        "env_int": 0.6, "drive": 0.4, "mod_int": 0.4, "vel_sens": 0.1,
        "attack1": 0.6, "decay1": 0.8, "sustain1": 0.9, "release1": 0.8,
        "attack2": 0.5, "decay2": 0.8, "sustain2": 0.95, "release2": 0.85,
        "lfo1_rate": 0.1, "lfo2_rate": 0.08, "chorus_mix": 0.5,
        "delay_time": 0.6, "delay_feedback": 0.65, "delay_mix": 0.4,
        "master_vol": 0.78, "pan": 0.5
    },
    "ring_lead": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.33, "detune": 0.68, "sync_ring": 0.66,
        "osc_mix": 0.6, "sub_level": 0.0, "portamento": 0.1,
        "cutoff": 0.6, "resonance": 0.6, "filter_type": 0.0, "keytrack": 0.6,
        "env_int": 0.7, "drive": 0.4, "mod_int": 0.3, "vel_sens": 0.3,
        "attack1": 0.02, "decay1": 0.4, "sustain1": 0.5, "release1": 0.25,
        "attack2": 0.015, "decay2": 0.4, "sustain2": 0.8, "release2": 0.25,
        "lfo1_rate": 0.4, "lfo2_rate": 0.35, "chorus_mix": 0.3,
        "delay_time": 0.3, "delay_feedback": 0.4, "delay_mix": 0.3,
        "master_vol": 0.8, "pan": 0.5
    },
    "texture": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.33, "detune": 0.58, "sync_ring": 0.66,
        "osc_mix": 0.5, "sub_level": 0.2, "portamento": 0.2,
        "cutoff": 0.45, "resonance": 0.75, "filter_type": 0.66, "keytrack": 0.4,
        "env_int": 0.65, "drive": 0.3, "mod_int": 0.5, "vel_sens": 0.3,
        "attack1": 0.3, "decay1": 0.6, "sustain1": 0.6, "release1": 0.6,
        "attack2": 0.25, "decay2": 0.6, "sustain2": 0.8, "release2": 0.65,
        "lfo1_rate": 0.2, "lfo2_rate": 0.15, "chorus_mix": 0.6,
        "delay_time": 0.5, "delay_feedback": 0.6, "delay_mix": 0.45,
        "master_vol": 0.75, "pan": 0.5
    },
    "sfx_laser": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.66, "detune": 0.8, "sync_ring": 0.33,
        "osc_mix": 0.5, "sub_level": 0.0, "portamento": 0.4,
        "cutoff": 0.7, "resonance": 0.85, "filter_type": 0.0, "keytrack": 0.3,
        "env_int": 0.95, "drive": 0.5, "mod_int": 0.4, "vel_sens": 0.4,
        "attack1": 0.001, "decay1": 0.2, "sustain1": 0.0, "release1": 0.15,
        "attack2": 0.001, "decay2": 0.25, "sustain2": 0.05, "release2": 0.15,
        "lfo1_rate": 0.7, "lfo2_rate": 0.8, "chorus_mix": 0.3,
        "delay_time": 0.3, "delay_feedback": 0.5, "delay_mix": 0.4,
        "master_vol": 0.8, "pan": 0.5
    },
    "sfx_drop": {
        "wave1": 1.0, "pulse_width": 0.5, "wave2": 0.66, "detune": 0.35, "sync_ring": 0.0,
        "osc_mix": 0.2, "sub_level": 0.9, "portamento": 0.35,
        "cutoff": 0.4, "resonance": 0.6, "filter_type": 0.0, "keytrack": 0.2,
        "env_int": 0.85, "drive": 0.6, "mod_int": 0.0, "vel_sens": 0.3,
        "attack1": 0.005, "decay1": 0.6, "sustain1": 0.0, "release1": 0.5,
        "attack2": 0.005, "decay2": 0.7, "sustain2": 0.1, "release2": 0.5,
        "lfo1_rate": 0.1, "lfo2_rate": 0.1, "chorus_mix": 0.1,
        "delay_time": 0.4, "delay_feedback": 0.6, "delay_mix": 0.35,
        "master_vol": 0.85, "pan": 0.5
    },
    "sfx_zap": {
        "wave1": 0.33, "pulse_width": 0.5, "wave2": 0.33, "detune": 0.7, "sync_ring": 0.66,
        "osc_mix": 0.6, "sub_level": 0.0, "portamento": 0.0,
        "cutoff": 0.6, "resonance": 0.8, "filter_type": 0.66, "keytrack": 0.5,
        "env_int": 0.9, "drive": 0.6, "mod_int": 0.5, "vel_sens": 0.5,
        "attack1": 0.001, "decay1": 0.15, "sustain1": 0.0, "release1": 0.1,
        "attack2": 0.001, "decay2": 0.18, "sustain2": 0.0, "release2": 0.1,
        "lfo1_rate": 0.8, "lfo2_rate": 0.6, "chorus_mix": 0.2,
        "delay_time": 0.25, "delay_feedback": 0.5, "delay_mix": 0.35,
        "master_vol": 0.8, "pan": 0.5
    },
    "sfx_sweep": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.33, "detune": 0.6, "sync_ring": 0.0,
        "osc_mix": 0.5, "sub_level": 0.2, "portamento": 0.2,
        "cutoff": 0.2, "resonance": 0.85, "filter_type": 0.66, "keytrack": 0.3,
        "env_int": 0.9, "drive": 0.5, "mod_int": 0.3, "vel_sens": 0.2,
        "attack1": 0.5, "decay1": 0.5, "sustain1": 0.3, "release1": 0.5,
        "attack2": 0.3, "decay2": 0.6, "sustain2": 0.7, "release2": 0.6,
        "lfo1_rate": 0.25, "lfo2_rate": 0.3, "chorus_mix": 0.4,
        "delay_time": 0.5, "delay_feedback": 0.65, "delay_mix": 0.45,
        "master_vol": 0.78, "pan": 0.5
    },
    "vocoder_formant": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.33, "detune": 0.52, "sync_ring": 0.0,
        "osc_mix": 0.4, "sub_level": 0.2, "portamento": 0.08,
        "cutoff": 0.48, "resonance": 0.78, "filter_type": 0.66, "keytrack": 0.7,
        "env_int": 0.7, "drive": 0.45, "mod_int": 0.2, "vel_sens": 0.4,
        "attack1": 0.02, "decay1": 0.35, "sustain1": 0.6, "release1": 0.25,
        "attack2": 0.015, "decay2": 0.4, "sustain2": 0.85, "release2": 0.25,
        "lfo1_rate": 0.3, "lfo2_rate": 0.25, "chorus_mix": 0.4,
        "delay_time": 0.3, "delay_feedback": 0.3, "delay_mix": 0.2,
        "master_vol": 0.82, "pan": 0.5
    },
    "vocoder_choir": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.0, "detune": 0.535, "sync_ring": 0.0,
        "osc_mix": 0.5, "sub_level": 0.15, "portamento": 0.05,
        "cutoff": 0.52, "resonance": 0.65, "filter_type": 0.66, "keytrack": 0.6,
        "env_int": 0.6, "drive": 0.2, "mod_int": 0.3, "vel_sens": 0.3,
        "attack1": 0.2, "decay1": 0.5, "sustain1": 0.75, "release1": 0.5,
        "attack2": 0.15, "decay2": 0.5, "sustain2": 0.9, "release2": 0.55,
        "lfo1_rate": 0.22, "lfo2_rate": 0.18, "chorus_mix": 0.65,
        "delay_time": 0.45, "delay_feedback": 0.4, "delay_mix": 0.3,
        "master_vol": 0.8, "pan": 0.5
    },
    "vocoder_talkbox": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.5, "detune": 0.51, "sync_ring": 0.33,
        "osc_mix": 0.5, "sub_level": 0.2, "portamento": 0.12,
        "cutoff": 0.45, "resonance": 0.75, "filter_type": 0.66, "keytrack": 0.65,
        "env_int": 0.75, "drive": 0.4, "mod_int": 0.25, "vel_sens": 0.4,
        "attack1": 0.01, "decay1": 0.3, "sustain1": 0.5, "release1": 0.2,
        "attack2": 0.01, "decay2": 0.35, "sustain2": 0.8, "release2": 0.2,
        "lfo1_rate": 0.35, "lfo2_rate": 0.3, "chorus_mix": 0.35,
        "delay_time": 0.32, "delay_feedback": 0.3, "delay_mix": 0.25,
        "master_vol": 0.82, "pan": 0.5
    },
    "vocoder_bass": {
        "wave1": 0.0, "pulse_width": 0.5, "wave2": 0.33, "detune": 0.5, "sync_ring": 0.0,
        "osc_mix": 0.35, "sub_level": 0.7, "portamento": 0.05,
        "cutoff": 0.35, "resonance": 0.65, "filter_type": 0.66, "keytrack": 0.5,
        "env_int": 0.72, "drive": 0.45, "mod_int": 0.0, "vel_sens": 0.4,
        "attack1": 0.008, "decay1": 0.3, "sustain1": 0.2, "release1": 0.15,
        "attack2": 0.008, "decay2": 0.35, "sustain2": 0.6, "release2": 0.15,
        "lfo1_rate": 0.25, "lfo2_rate": 0.2, "chorus_mix": 0.2,
        "delay_time": 0.25, "delay_feedback": 0.1, "delay_mix": 0.1,
        "master_vol": 0.85, "pan": 0.5
    },
    "vocoder_whisper": {
        "wave1": 0.33, "pulse_width": 0.5, "wave2": 0.66, "detune": 0.55, "sync_ring": 0.0,
        "osc_mix": 0.5, "sub_level": 0.0, "portamento": 0.0,
        "cutoff": 0.65, "resonance": 0.6, "filter_type": 1.0, "keytrack": 0.5,
        "env_int": 0.6, "drive": 0.2, "mod_int": 0.2, "vel_sens": 0.3,
        "attack1": 0.1, "decay1": 0.5, "sustain1": 0.6, "release1": 0.4,
        "attack2": 0.08, "decay2": 0.5, "sustain2": 0.7, "release2": 0.45,
        "lfo1_rate": 0.3, "lfo2_rate": 0.2, "chorus_mix": 0.5,
        "delay_time": 0.4, "delay_feedback": 0.45, "delay_mix": 0.35,
        "master_vol": 0.78, "pan": 0.5
    }
}

presets_list = []
c_entries = []

for idx, (bank, genre_idx, prog_num, name, cat, desc, arch_key) in enumerate(PATCH_SPECS):
    # Verify index formula: (bank_side * 64) + (genre_category * 8) + (program_num - 1)
    bank_side = 0 if bank == "A" else 1
    expected_idx = (bank_side * 64) + (genre_idx * 8) + (prog_num - 1)
    assert idx == expected_idx, f"Index mismatch at {idx}: calculated {expected_idx}"

    code = f"{bank}.{genre_idx + 1}{prog_num}"
    full_name = f"{code} {name}"
    
    # Get base archetype params
    arch = ARCHETYPES.get(arch_key, ARCHETYPES["trance_lead"])
    params = {}
    for p in PARAM_NAMES:
        val = arch.get(p, 0.5)
        # Apply slight deterministic variations based on program number so each patch is unique
        if p == "cutoff":
            val = max(0.1, min(0.95, val + ((prog_num - 4.5) * 0.02)))
        elif p == "resonance":
            val = max(0.05, min(0.9, val + (((prog_num % 3) - 1) * 0.03)))
        elif p == "decay1":
            val = max(0.05, min(0.9, val + (((prog_num % 4) - 1.5) * 0.02)))
        params[p] = round(val, 4)

    presets_list.append({
        "id": idx,
        "bank": f"Side {bank}",
        "genre": GENRE_SHORT[genre_idx],
        "program": prog_num,
        "code": code,
        "name": full_name,
        "category": cat,
        "description": desc,
        "params": params
    })

    # C array representation
    p_vals = ", ".join(f"{params[p]:.4f}f" for p in PARAM_NAMES)
    c_entries.append(f'    /* [{idx:3d}] {code} {name} */\n    {{ "{full_name}", "{cat}",\n      {{ {p_vals} }} }}')

# Write src/presets.json
out_json_path = os.path.join("src", "presets.json")
with open(out_json_path, "w", encoding="utf-8") as f:
    json.dump({"presets": presets_list}, f, indent=2)

print(f"Generated {len(presets_list)} presets in {out_json_path}")

# Write src/dsp/presets_data.h
out_h_path = os.path.join("src", "dsp", "presets_data.h")
with open(out_h_path, "w", encoding="utf-8") as f:
    f.write("/* Auto-generated 128 microKORG Preset Definitions */\n")
    f.write("#ifndef PRESETS_DATA_H\n")
    f.write("#define PRESETS_DATA_H\n\n")
    f.write('#include "dsp.h"\n\n')
    f.write("#define NUM_PRESETS_128 128\n\n")
    f.write("static const preset_t PRESET_TABLE_128[NUM_PRESETS_128] = {\n")
    f.write(",\n".join(c_entries))
    f.write("\n};\n\n")
    f.write("#endif /* PRESETS_DATA_H */\n")

print(f"Generated C header with 128 presets in {out_h_path}")
