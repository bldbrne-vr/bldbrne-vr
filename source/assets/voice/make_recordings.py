# SPDX-FileCopyrightText: 2026 BB-VR contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
# Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
# You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
# keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
# licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
"""ElevenLabs recordings of lines.md (2026-11-23: "we generate recordings and put them in
game"). Each voice is designed once from its Voice Design prompt (an original voice; voices.json
keeps the ids), then every line is spoken into dry/<key>.mp3. After reverb, package.ps1
ships the approved mixes beside shadps4.exe (voice/). The lines' [tags] are Eleven v3/v4 audio tags (emotion, laughs,
whispers); the game never reads the words.

The API key comes from ELEVENLABS_API_KEY or --key-file. It is never written anywhere and is sent
only to api.elevenlabs.io, as the xi-api-key header.

usage (python -I keeps the folder's files off the import path):
  python -I make_recordings.py [--key-file F] status
  python -I make_recordings.py [--key-file F] design [--voices "Old hunter,Beast"]
  python -I make_recordings.py [--key-file F] add --voice "Old hunter" --library OWNER/VOICE
  python -I make_recordings.py [--key-file F] speak [--only boom_hammer,tonitrus] [--force]
Recordings go to dry/<key>.mp3; add_reverb.py makes the game's <key>.mp3 from them.
"""
import argparse
import base64
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
API = 'https://api.elevenlabs.io'
VOICES_FILE = HERE / 'voices.json'
DESIGN_MODEL = 'eleven_ttv_v3'
OUTPUT = 'mp3_44100_128'
# Per voice: speaking speed (0.7..1.2) and stability (lower is more expressive). Unhurried (the
# user: "maybe 18-20s so it doesn't feel rushed").
DELIVERY = {
    'Powder Kegs smith': (0.97, 0.4),
    'Old hunter': (0.9, 0.5),
    'Grizzled hunter': (0.9, 0.45),
    'Brooding hunter': (0.92, 0.45),
    'Regretful nun': (1.0, 0.45),
    'Church cleric': (0.92, 0.5),
    'Executioner': (0.95, 0.35),
    'Church assassin': (0.95, 0.4),
    'Mad tinkerer': (1.0, 0.3),
    'Cainhurst noble': (0.92, 0.5),
    'Beast': (0.9, 0.3),
    'Hushed scholar': (0.92, 0.4),
    'Drowned fisherman': (0.92, 0.35),
}


def api_key(args):
    if args.key_file:
        return Path(args.key_file).read_text(encoding='utf-8').strip()
    key = os.environ.get('ELEVENLABS_API_KEY', '').strip()
    if not key:
        sys.exit('Give --key-file or set ELEVENLABS_API_KEY.')
    return key


class Missing(Exception):
    """The key lacks a permission (a restricted key)."""


def call(key, method, path, body=None, accept='application/json', retries=3, optional=False):
    data = json.dumps(body).encode('utf-8') if body is not None else None
    for attempt in range(retries):
        request = urllib.request.Request(API + path, data=data, method=method, headers={
            'xi-api-key': key, 'Content-Type': 'application/json', 'Accept': accept,
            'User-Agent': 'bbvr-voice-tool/1.0'})
        try:
            with urllib.request.urlopen(request, timeout=180) as response:
                payload = response.read()
            return json.loads(payload) if accept == 'application/json' else payload
        except urllib.error.HTTPError as e:
            detail = e.read().decode('utf-8', 'replace')[:600]
            if optional and e.code in (401, 403) and 'missing_permissions' in detail:
                raise Missing(detail)
            if e.code in (409, 429, 500, 502, 503) and attempt + 1 < retries:
                time.sleep(5 * (attempt + 1))
                continue
            sys.exit(f'{method} {path}: HTTP {e.code}: {detail}')
        except urllib.error.URLError as e:
            if attempt + 1 < retries:
                time.sleep(5)
                continue
            sys.exit(f'{method} {path}: {e.reason}')


def tables():
    """lines.md: the Voices table (name -> prompt) and the Lines table ([(key, voice, line)])."""
    voices, lines, section = {}, [], None
    for row in (HERE / 'lines.md').read_text(encoding='utf-8').splitlines():
        if row.startswith('## '):
            section = row[3:].strip()
            continue
        cells = [c.strip() for c in row.strip().strip('|').split('|')] if row.startswith('|') else []
        if not cells or set(cells[0]) <= set('-') or cells[0] in ('voice', 'key'):
            continue
        if section == 'Voices' and len(cells) == 2:
            voices[cells[0]] = cells[1]
        elif section == 'Lines' and len(cells) == 3:
            lines.append(tuple(cells))
    return voices, lines


def plain(text):
    """A line without its audio tags (for a voice's design sample)."""
    return re.sub(r'\s*\[[^\]]*\]\s*', ' ', text).strip()


def status(key):
    try:
        s = call(key, 'GET', '/v1/user/subscription', optional=True)
        print(f"tier {s.get('tier')} ({s.get('status')}): characters {s.get('character_count')} of "
              f"{s.get('character_limit')} used; can extend: {s.get('can_extend_character_limit')}")
        print(f"voice slots {s.get('voice_slots_used')} of {s.get('voice_limit')} used; "
              f"add/edit counter {s.get('voice_add_edit_counter')} of {s.get('max_voice_add_edits')}")
    except Missing:
        print('subscription: not readable with this key (no user_read permission)')
    try:
        models = call(key, 'GET', '/v1/models', optional=True)
        names = [m.get('model_id') for m in models if m.get('can_do_text_to_speech')]
        print('text-to-speech models:', ', '.join(n for n in names if n))
    except Missing:
        print('models: not readable with this key')
    try:
        listed = call(key, 'GET', '/v2/voices?page_size=100&voice_type=personal', optional=True)
        mine = [v.get('name') for v in listed.get('voices', [])]
        print(f'personal voices: {len(mine)}' + (f" ({', '.join(mine[:12])})" if mine else ''))
    except Missing:
        print('voices: not readable with this key (no voices_read permission)')
    saved = json.loads(VOICES_FILE.read_text(encoding='utf-8')) if VOICES_FILE.exists() else {}
    voices, lines = tables()
    print(f'{len(voices)} voices in lines.md ({len(saved)} designed), {len(lines)} lines, '
          f'{sum(len(t) for _, _, t in lines)} characters to speak; '
          f"{sum((HERE / 'dry' / (k + '.mp3')).exists() for k, _, _ in lines)} recorded")


def design(key, only):
    voices, lines = tables()
    saved = json.loads(VOICES_FILE.read_text(encoding='utf-8')) if VOICES_FILE.exists() else {}
    for name, prompt in voices.items():
        if (only and name not in only) or (name in saved and not only):
            continue
        # The sample the previews speak (100..1000 characters; kept short, as it costs credits):
        # this voice's own lines, the first ones that reach 100.
        sample = ''
        for text in [plain(t) for _, v, t in lines if v == name]:
            sample = (sample + ' ' + text).strip()
            if len(sample) >= 100:
                break
        while len(sample) < 100:
            sample = (sample + ' ' + sample).strip()
        result = call(key, 'POST', '/v1/text-to-voice/design', {
            'voice_description': prompt, 'model_id': DESIGN_MODEL, 'text': sample})
        preview = result['previews'][0]
        voice = call(key, 'POST', '/v1/text-to-voice', {
            'voice_name': f'BBVR {name}', 'voice_description': prompt,
            'generated_voice_id': preview['generated_voice_id'],
            'labels': {'project': 'bloodborne-vr'}})
        saved[name] = voice['voice_id']
        VOICES_FILE.write_text(json.dumps(saved, indent=1) + '\n', encoding='utf-8')
        print(f'designed {name}: {voice["voice_id"]} ({preview["duration_secs"]:.1f} s preview)')


def add(key, voice, library, label=''):
    """A voice from the ElevenLabs Voice Library (library: <public owner id>/<voice id>) as this
    character's (it takes a voice slot): voices.json keeps its id in this account."""
    owner, voice_id = library.split('/', 1)
    name = f'BBVR {voice} ({label})' if label else f'BBVR {voice}'
    added = call(key, 'POST', f'/v1/voices/add/{owner}/{voice_id}',
                 {'new_name': name, 'bookmarked': True})
    saved = json.loads(VOICES_FILE.read_text(encoding='utf-8')) if VOICES_FILE.exists() else {}
    saved[voice] = added['voice_id']
    VOICES_FILE.write_text(json.dumps(saved, indent=1) + '\n', encoding='utf-8')
    print(f'added {voice}: {added["voice_id"]} (library {voice_id})')


def speak(key, only, force, model):
    voices, lines = tables()
    saved = json.loads(VOICES_FILE.read_text(encoding='utf-8')) if VOICES_FILE.exists() else {}
    (HERE / 'dry').mkdir(exist_ok=True)
    for line_key, voice, text in lines:
        out = HERE / 'dry' / f'{line_key}.mp3' # add_reverb.py makes the game's <key>.mp3 of it
        if (only and line_key not in only) or (out.exists() and not force and not only):
            continue
        if voice not in saved:
            sys.exit(f'{voice} is not designed yet: run design first.')
        speed, stability = DELIVERY.get(voice, (1.0, 0.5))
        audio = call(key, 'POST', f'/v1/text-to-speech/{saved[voice]}?output_format={OUTPUT}', {
            'text': text, 'model_id': model,
            'voice_settings': {'stability': stability, 'similarity_boost': 0.75, 'style': 0.0,
                               'use_speaker_boost': True, 'speed': speed}},
            accept='audio/mpeg')
        out.write_bytes(audio)
        print(f'spoke {line_key} ({voice}): {len(audio) // 1024} KB')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--key-file')
    parser.add_argument('command', choices=['status', 'design', 'add', 'speak'])
    parser.add_argument('--voices', default='')
    parser.add_argument('--voice', default='')
    parser.add_argument('--library', default='')
    parser.add_argument('--label', default='')
    parser.add_argument('--only', default='')
    parser.add_argument('--force', action='store_true')
    parser.add_argument('--model', default='eleven_v3')
    args = parser.parse_args()
    key = api_key(args)
    if args.command == 'status':
        status(key)
    elif args.command == 'design':
        design(key, {v.strip() for v in args.voices.split(',') if v.strip()})
    elif args.command == 'add':
        if not args.voice or '/' not in args.library:
            sys.exit('add needs --voice "<character>" --library <public owner id>/<voice id>')
        add(key, args.voice, args.library, args.label)
    else:
        speak(key, {k.strip() for k in args.only.split(',') if k.strip()}, args.force, args.model)


if __name__ == '__main__':
    main()
