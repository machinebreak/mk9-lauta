"""The character select's 3D fighters (app/src/mk9_select_preview.cpp): their performances for the pick, read from the
game's own cinematics, written to app/src/mk9_select_performances.inc.

  - The Story transitions (Asset/NIS_MK<nnn>_<PRE|PST>.xxx): before and after each Story fight one fighter speaks to
    the other. Its group's face track names it ("..._<Who>_FO"); its body and face animations start at their keys;
    its line is snd_vo_storytrans.vo_mk9_<nnn>_<pre|pst> (SND_VO_StoryTrans<_lang>: English, Spanish, French, German,
    Italian) at its sound key.
  - The fighters' intros (Asset/<Fighter>_CharIntro.xxx), as before a fight: the body and face animations, and every
    sound key: the sound events (InterpTrackSound: "PACKAGE.event", the intro's line snd_vo_charintro.vo_char_intro_*
    among them) and the character's voice sounds (CinemaTrackCharacterSound, voice type: "@vo_x", the fighter's own).

  - The Story cutscenes' lines (Asset/SND_MOV_MK<nnn>.xxx, a dub each: _spa, _fra, _ger, _ita): one event a line,
    named after who says it ("vo_mk006_sony02": Sonya); the select says one with the fighter's EndOfRound_Taunt.

A cinematic plays its main animation in segments (its cuts) and its props' and the stance's beside it: the character's
main animations count once each, from their first start; the props' (swords, knives, the gun, ...) are left out.

Some walk away (the cinematic moves the character, its animation its hips): a fighter in its place on the select
screen would end elsewhere and jump back to its stance. The game measures them (mk9_select_preview_test_survey: each
one's hips along the floor from where they are in the stance, at its first frames, the farthest and at its end); with
that log the ones that end farther than --max-end units from their place, or last longer than --max-length seconds,
are left out. One that comes in from out of sight (an intro's entrance) and ends in its place stays.

Usage: perf_tables.py <game's Asset dir> [<out .inc>] [--survey <its log> [--max-end 40] [--max-length 16]]
       (default out: app/src/mk9_select_performances.inc)
"""
import glob
import os
import re
import sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import pkg_any  # noqa: E402
import props  # noqa: E402

Model = pkg_any.Model

SPEAKERS = {  # a Story speaker (its face animation's) -> the select's fighter; None: not in the select
    'JohnnyCage': 'johnnycage', 'Baraka': 'baraka', 'Sonya': 'sonya', 'Kano': 'kano', 'Scorpion': 'scorpion',
    'Cyrax_B': 'cyrax', 'Cyrax': 'cyrax', 'Sheeva': 'sheeva', 'Ermac': 'ermac', 'LiuKang': 'liukang',
    'Kitana': 'kitana', 'Kitana_B': 'kitana', 'ShangTsung': 'shangtsung', 'Jax': 'jax', 'Smoke': 'smoke',
    'Smoke_B': 'smoke', 'SubZero': 'subzero', 'Reptile': 'reptile', 'Jade': 'jade', 'Jade_B': 'jade',
    'Mileena': 'mileena', 'Noob': 'noob', 'KungLao': 'kunglao', 'Stryker': 'stryker', 'Stryker_B': 'stryker',
    'Kabal': 'kabal', 'Nightwolf': 'nightwolf', 'Sindel': 'sindel', 'Raiden': 'raiden',
    'Goro': None, 'Kintaro': None, 'ShaoKahn': None,
}
INTROS = {  # <Fighter>_CharIntro -> the select's fighter (the Story's and the bosses' variants left out)
    'Baraka': 'baraka', 'CyberZero': 'cyberzero', 'Cyrax': 'cyrax', 'Ermac': 'ermac',
    'FreddyKrueger': 'freddykrueger', 'Jade': 'jade', 'Jax': 'jax', 'JohnnyCage': 'johnnycage', 'Kabal': 'kabal',
    'Kano': 'kano', 'Kenshi': 'kenshi', 'Kitana': 'kitana', 'KungLao': 'kunglao', 'LiuKang': 'liukang',
    'Mileena': 'mileena', 'Nightwolf': 'nightwolf', 'NoobSaibot': 'noob', 'QuanChi': 'quanchi', 'Raiden': 'raiden',
    'Rain': 'rain', 'Reptile': 'reptile', 'Scorpion': 'scorpion', 'Sektor': 'sektor', 'ShangTsung': 'shangtsung',
    'Sheeva': 'sheeva', 'Sindel': 'sindel', 'Skarlet': 'skarlet', 'Smoke': 'smoke', 'Sonya': 'sonya',
    'Stryker': 'stryker', 'Subzero': 'subzero',
}
LINES = {  # a cutscene line's speaker (its event's code) -> the select's fighter (Shao Kahn, Goro: not in the select)
    'sony': 'sonya', 'jade': 'jade', 'kita': 'kitana', 'john': 'johnnycage', 'shee': 'sheeva', 'raid': 'raiden',
    'stry': 'stryker', 'bara': 'baraka', 'nigh': 'nightwolf', 'kano': 'kano', 'cyrh': 'cyrax', 'sekh': 'sektor',
    'sekt': 'sektor', 'jax': 'jax', 'sub': 'subzero', 'liuk': 'liukang', 'mile': 'mileena', 'scor': 'scorpion',
    'smok': 'smoke', 'kaba': 'kabal',
}
PROPS = re.compile(r'(sword|blade|knife|_gun_|bomb|boom|_rt_|_lf_|_hat|head_zero|body_zero|arm_zero|leg_zero|blades_in|'
                   r'mask$)', re.I)


def class_of(m, x):
    return m.class_path(x['w'][0]).split('.')[-1]


def read(m, x):
    try:
        return props.Reader(m, x['data']).props()
    except Exception:  # noqa: BLE001
        return []


def get(ps, name, default=None):
    for n, _, v in ps:
        if n == name:
            return v
    return default


def items(v):
    return v[1] if isinstance(v, tuple) and v and v[0] == 'array' else []


def tracks(m):
    """The animation keys (name, start, subtrack), the sound events (time, "PACKAGE.event") and the character's voice
    sounds (time, "vo_x")."""
    anims, events, voices = [], [], []
    for x in m.exports:
        c = class_of(m, x)
        if c == 'CinemaTrackBlendAnim':
            for e in items(get(read(m, x), 'AnimSeqs')):
                anims.append((get(e, 'AnimSeqName'), round(get(e, 'StartTime', 0.0), 3), get(e, 'SubTrack', 0)))
        elif c == 'InterpTrackSound':
            for e in items(get(read(m, x), 'Sounds')):
                if get(e, 'EventName'):
                    events.append((round(get(e, 'Time', 0.0), 3), get(e, 'EventName')))
        elif c == 'CinemaTrackCharacterSound':
            for e in items(get(read(m, x), 'CharacterSoundTrack')):
                name = get(e, 'SoundName')
                if name and name.startswith('vo_') and get(e, 'SoundTrackType', 0) == 0:
                    voices.append((round(get(e, 'Time', 0.0), 3), name))
    return anims, events, voices


def split(anims):
    """The face animations, and the character's main body animations (each from its first start)."""
    face, body = [], []
    for a in anims:
        if not a[0]:
            continue
        if a[0].lower().endswith('_fo') or 'facial' in a[0].lower():
            if a[0] not in [f[0] for f in face]:
                face.append(a)
            continue
        if a[0].lower().startswith(('stance', 'idle')) or PROPS.search(a[0]):
            continue
        if not re.match(r'^(MK\d{3}|MK9_\d{3}|MK_\d{3}|MK9_Intro)', a[0], re.I):
            continue
        same = [b for b in body if b[0] == a[0]]
        if same:
            if a[1] < same[0][1]:
                body[body.index(same[0])] = a
            continue
        body.append(a)
    if len(body) > 1:  # Cyrax's intro: its parts' animations and the main one
        main = [b for b in body if 'main' in b[0].lower()]
        if main:
            body = main
    return sorted(body, key=lambda b: b[1]), face


def survey_results(path):
    """The survey's log: each performance's package -> (from its place at first, farthest, at its end, its length)."""
    out = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        found = re.search(r'\[select3d\] survey \d+ [SI] (\S+): from its place first (\d+), farthest (\d+) at [\d.]+ s, '
                          r'at the end (\d+) \(([\d.]+) s\)', line)
        if found:
            out[found.group(1)] = (int(found.group(2)), int(found.group(3)), int(found.group(4)),
                                   float(found.group(5)))
    return out


def option(args, name, default, kind):
    if name not in args:
        return default
    i = args.index(name)
    value = kind(args[i + 1])
    del args[i:i + 2]
    return value


def main():
    args = list(sys.argv[1:])
    survey_log = option(args, '--survey', None, str)
    max_end = option(args, '--max-end', 40, int)
    max_length = option(args, '--max-length', 16.0, float)
    survey = survey_results(survey_log) if survey_log else None
    asset = args[0]
    out = args[1] if len(args) > 1 else os.path.join(here, '..', '..', 'app', 'src', 'mk9_select_performances.inc')
    rows = []
    for path in sorted(glob.glob(os.path.join(asset, 'NIS_MK*_P*.xxx'))):
        m = Model.load(path)
        anims, events, voices = tracks(m)
        body, face = split(anims)
        who = None
        for a in face:
            found = re.search(r'_([A-Za-z]+(?:_B)?)_FO$', a[0])
            if found and found.group(1) in SPEAKERS:
                who = found.group(1)
        fighter = SPEAKERS.get(who) if who else None
        if not fighter or not body or not any('storytrans' in e.lower() for _, e in events):
            continue
        rows.append((fighter, 'S', os.path.basename(path)[:-4], body, face[0] if face else None,
                     sorted(events + [(t, '@' + v) for t, v in voices])))
    for path in sorted(glob.glob(os.path.join(asset, '*_CharIntro.xxx'))):
        fighter = INTROS.get(os.path.basename(path)[:-len('_CharIntro.xxx')])
        if not fighter:
            continue
        m = Model.load(path)
        anims, events, voices = tracks(m)
        body, face = split(anims)
        if body:
            rows.append((fighter, 'I', os.path.basename(path)[:-4], body, face[0] if face else None,
                         sorted(events + [(t, '@' + v) for t, v in voices])))
    for path in sorted(glob.glob(os.path.join(asset, 'SND_MOV_MK[0-9][0-9][0-9].xxx'))):
        m = Model.load(path)
        package = os.path.basename(path)[:-4]
        for i, x in enumerate(m.exports):
            if class_of(m, x) != 'FmodEvent':
                continue
            event = m.path(i + 1).split('.')[-1]
            found = re.match(r'^vo_mk9?_?\d{3}_([a-z]+?)\d+[a-z]?$', event)  # (not the sighs and growls)
            fighter = LINES.get(found.group(1)) if found else None
            if fighter:
                rows.append((fighter, 'L', package, [], None, [(0.3, '%s.%s' % (package, event))]))
    walking, long_ones = [], []
    if survey is not None:
        walking = [r for r in rows if r[2] in survey and survey[r[2]][2] > max_end]
        long_ones = [r for r in rows if r[2] in survey and survey[r[2]][3] > max_length and r not in walking]
        unmeasured = [r[2] for r in rows if r[2] not in survey and r[1] != 'L']
        if unmeasured:
            print('not in the survey (kept):', ' '.join(unmeasured))
        rows = [r for r in rows if r not in walking and r not in long_ones]
    lines = [
        '// mk9-lauta (2026-11-24): the character select\'s performances for the pick (mk9_select_preview.cpp),',
        '// generated by tools/select3d/perf_tables.py from the game\'s cinematics: do not edit by hand.',
        '// {fighter, \'S\' Story transition / \'I\' intro / \'L\' a cutscene\'s line, the package to load (a line\'s',
        '//  in the game\'s language), "body@start|..." (a line\'s: the fighter\'s EndOfRound_Taunt), "face@start",',
        '//  "time:PACKAGE.event;time:@vo_x;..."} (times in seconds of the cinematic)',
    ]
    if walking:
        lines.append('// Left out, ending more than %d units from their place (the survey): %s' % (
            max_end, ' '.join(sorted(r[2] for r in walking))))
    if long_ones:
        lines.append('// Left out, longer than %g s: %s' % (max_length, ' '.join(sorted(r[2] for r in long_ones))))
    for fighter, kind, package, body, face, cues in rows:
        lines.append('{"%s", \'%s\', "%s", "%s", "%s",\n "%s"},' % (
            fighter, kind, package, '|'.join('%s@%.3f' % (n, t) for n, t, _ in body),
            '%s@%.3f' % (face[0], face[1]) if face else '', ';'.join('%.3f:%s' % (t, e) for t, e in cues)))
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    kinds = {k: sum(1 for r in rows if r[1] == k) for k in 'SIL'}
    print('%s: %d Story transitions, %d intros, %d cutscene lines, %d fighters' % (
        os.path.normpath(out), kinds['S'], kinds['I'], kinds['L'], len({r[0] for r in rows})))


if __name__ == '__main__':
    main()
