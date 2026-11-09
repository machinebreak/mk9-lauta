#!/usr/bin/env python3
"""Signs a release archive for the in-game updater (app/src/mk9_updater.h): writes <archive>.sig, the release key's
ECDSA P-256 signature of the archive's SHA-256, r and s as 64 bytes in hex. The updater refuses an archive whose
signature does not match the public key built into the program.

    python tools/release/sign_release.py <archive> [--key <private key .pem>]

The private key: --key, or $MK9_LAUTA_SIGNING_KEY, or _keys/mk9-lauta-update-key.pem in the folder that holds the
repository (never inside it). Keep a copy of it somewhere safe: without it no later release can be installed by the
updater (players would have to update by hand once, to a program with a new key).
"""
import argparse
import os
import sys

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('archive')
    parser.add_argument('--key', default=os.environ.get('MK9_LAUTA_SIGNING_KEY') or
                        os.path.join(os.path.dirname(REPO), '_keys', 'mk9-lauta-update-key.pem'))
    args = parser.parse_args()
    if not os.path.exists(args.key):
        sys.exit(f'no signing key at {args.key}')
    with open(args.key, 'rb') as f:
        key = serialization.load_pem_private_key(f.read(), None)
    with open(args.archive, 'rb') as f:
        data = f.read()
    r, s = decode_dss_signature(key.sign(data, ec.ECDSA(hashes.SHA256())))
    signature = r.to_bytes(32, 'big') + s.to_bytes(32, 'big')
    # check it before it goes out
    key.public_key().verify(key.sign(data, ec.ECDSA(hashes.SHA256())), data, ec.ECDSA(hashes.SHA256()))
    with open(args.archive + '.sig', 'w') as f:
        f.write(signature.hex() + '\n')
    print(f'{args.archive}.sig')


if __name__ == '__main__':
    main()
