#!/usr/bin/env python3
"""Select or record verified prebuilt AppImage image digests.

``select`` prints the verified ``repository@sha256:...`` reference for one
target from a JSON object produced by the token-holding verifier job, and
fails for anything that is not an exact digest reference in the repository.
``record`` validates one verified reference and adds it to that JSON object.
"""
import argparse
import json
import re
import sys

TARGETS = ('build', 'smoke-ubuntu24', 'smoke-ubuntu26', 'smoke-debian13', 'graphical')


def digest_reference(repository, value):
    prefix = f'{repository}@sha256:'
    if not (isinstance(value, str) and value.startswith(prefix)
            and re.fullmatch(r'[0-9a-f]{64}', value[len(prefix):])):
        raise ValueError('not an exact digest reference in the prebuilt repository')
    return value


def load_images(text):
    images = json.loads(text) if text else {}
    if not isinstance(images, dict) or any(key not in TARGETS for key in images):
        raise ValueError('prebuilt image map is invalid')
    return images


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('select', 'record'))
    parser.add_argument('--repository', required=True)
    parser.add_argument('--target', required=True, choices=TARGETS)
    parser.add_argument('--images', default='')
    parser.add_argument('--reference', default='')
    args = parser.parse_args(argv)
    try:
        images = load_images(args.images)
        if args.action == 'select':
            print(digest_reference(args.repository, images.get(args.target)))
        else:
            images[args.target] = digest_reference(args.repository, args.reference)
            print(json.dumps(images, sort_keys=True, separators=(',', ':')))
    except (ValueError, json.JSONDecodeError) as error:
        print(f'prebuilt image {args.action} failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
