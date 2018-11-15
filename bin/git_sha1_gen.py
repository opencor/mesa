#!/usr/bin/env python

"""
Generate the contents of the git_sha1.h file.
The output of this script goes to stdout.
"""


import argparse
import os
import os.path
import subprocess
import sys


def get_git_sha1():
    """Try to get the git SHA1 with git rev-parse."""
    git_dir = os.path.join(os.path.dirname(sys.argv[0]), '..', '.git')
    try:
        git_sha1 = subprocess.check_output([
            'git',
            '--git-dir=' + git_dir,
            'rev-parse',
            'HEAD',
        ], stderr=open(os.devnull, 'w')).decode("ascii")
    except:
        # don't print anything if it fails
        git_sha1 = ''
    return git_sha1

parser = argparse.ArgumentParser()
parser.add_argument('--output', help='File to write the #define in',
        required=True)
args = parser.parse_args()

git_sha1 = os.environ.get('MESA_GIT_SHA1_OVERRIDE', get_git_sha1())[:10]
if git_sha1:
    git_sha1_h_in_path = os.path.join(os.path.dirname(sys.argv[0]),
            '..', 'src', 'git_sha1.h.in')
    with open(git_sha1_h_in_path , 'r') as git_sha1_h_in:
        new_sha1 = git_sha1_h_in.read().replace('@VCS_TAG@', git_sha1)
        if os.path.isfile(args.output):
            with open(args.output, 'r') as git_sha1_h:
                if git_sha1_h.read() == new_sha1:
                    quit()
        with open(args.output, 'w') as git_sha1_h:
            git_sha1_h.write(new_sha1)
else:
    open(args.output, 'w').close()
