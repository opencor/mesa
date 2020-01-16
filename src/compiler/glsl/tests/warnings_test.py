# encoding=utf-8
# Copyright © 2017 Intel Corporation

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

from __future__ import print_function
import argparse
import errno
import os
import subprocess
import sys


def arg_parser():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '--glsl-compiler',
        required=True,
        help='Path to the standalone glsl compiler')
    parser.add_argument(
        '--test-directory',
        required=True,
        help='Directory containing tests to run.')
    return parser.parse_args()


def get_test_runner(runner):
    """Wrap the test runner in the exe wrapper if necessary."""
    wrapper =  os.environ.get('MESON_EXE_WRAPPER', None)
    if wrapper is None:
        return [runner]
    return [wrapper, runner]


def main():
    args = arg_parser()
    files = [f for f in os.listdir(args.test_directory) if f.endswith('.vert')]
    passed = 0

    if not files:
        print('Could not find any tests')
        exit(1)

    runner = get_test_runner(args.glsl_compiler)

    print('====== Testing compilation output ======')
    for file in files:
        print('Testing {} ...'.format(file), end='')
        file = os.path.join(args.test_directory, file)

        with open('{}.expected'.format(file), 'rb') as f:
            expected = f.read().strip()

        actual = subprocess.check_output(
            runner + ['--just-log', '--version', '150', file]
        ).strip()

        if actual == expected:
            print('PASS')
            passed += 1
        else:
            print('FAIL')

    print('{}/{} tests returned correct results'.format(passed, len(files)))
    exit(0 if passed == len(files) else 1)


if __name__ == '__main__':
    try:
        main()
    except OSError as e:
        if e.errno == errno.ENOEXEC:
            print('Skipping due to lack of exe_wrapper.', file=sys.stderr)
            sys.exit(77)
        else:
            raise
