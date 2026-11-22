"""A test root with built costumes: make_test_root's registration, then each costume's sections set from
build_costume.COSTUMES (the look's mesh, expressions, the Babality's mesh), then the TOC sizes.

    python setup_costumes.py <game_root> <test_root> <built folder> <costume> [...]
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from build_costume import COSTUMES  # noqa: E402
from build_default import COSTUMES as DEFAULT_BASED  # noqa: E402


def main():
    game, test, built = sys.argv[1], sys.argv[2], sys.argv[3]
    keys = sys.argv[4:]
    args = []
    for key in keys:
        if key in DEFAULT_BASED:
            spec = DEFAULT_BASED[key]
            name, new = spec['char'], key.split('_')[-1]
            args.append('%s=%s:%s' % (key, os.path.join(built, 'CHAR_%s_%s.xxx' % (name, new.upper())),
                                      os.path.join(built, 'NPC_%s_%s_Baby.xxx' % (name, new.upper()))))
            continue
        spec = COSTUMES[key]
        name, base, new = spec['char'], spec['base'], key.split('_')[-1]
        args.append('%s=%s:%s:%s_%s' % (key, os.path.join(built, 'CHAR_%s_%s.xxx' % (name, new.upper())),
                                         os.path.join(built, 'NPC_%s_%s_Baby.xxx' % (name, new.upper())),
                                         key.split('_')[0], base))
    py = sys.executable
    subprocess.check_call([py, '-I', os.path.join(HERE, 'make_test_root.py'), game, test] + args)
    for key in keys:
        if key in DEFAULT_BASED:
            spec = DEFAULT_BASED[key]
            name, letter, look = spec['char'], key.split('_')[-1].upper(), spec['look']
            costume = [('SourceAsset', 'CHAR_' + look), ('%s_Expressions' % name, look + '_Expressions'),
                       ('SourceMapping', 'CHAR_' + name), ('MeshName', look),
                       ('BabyName', 'NPC_%s_%s_Baby' % (name, letter)),
                       ('BabyScript', 'NPC_%s_%s_Baby.mko' % (name, letter))]
            subprocess.check_call([py, '-I', os.path.join(HERE, 'ini_set.py'), test, '[CHAR_%s]' % key.upper()] +
                                  ['%s=%s' % kv for kv in costume])
            subprocess.check_call([py, '-I', os.path.join(HERE, 'ini_set.py'), test, '[NPC_%s_BABY]' % key.upper(),
                                   'SourceAsset=NPC_%s_Baby' % look, 'MeshName=%s_Baby' % look])
            continue
        spec = COSTUMES[key]
        name = spec['char']
        ini = {k.replace('{Char}', name.capitalize() if name != 'SubZero' else 'Subzero'): v
               for k, v in spec['ini'].items()}
        sec = '[CHAR_%s]' % key.upper()
        subprocess.check_call([py, '-I', os.path.join(HERE, 'ini_set.py'), test, sec] +
                              ['%s=%s' % kv for kv in ini.items()])
        bsec = '[NPC_%s_BABY]' % key.upper()
        subprocess.check_call([py, '-I', os.path.join(HERE, 'ini_set.py'), test, bsec] +
                              ['%s=%s' % kv for kv in spec['baby_ini'].items()])
    subprocess.check_call([py, '-I', os.path.join(HERE, 'fix_toc.py'), test])


if __name__ == '__main__':
    main()
