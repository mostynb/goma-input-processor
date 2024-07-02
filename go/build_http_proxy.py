#!/usr/bin/env python3
# Copyright 2020 The Goma Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Script to build HTTP/1.1 to HTTP/2.0 proxy."""

import argparse
import os
import subprocess
import sys

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_TOP_DIR = os.path.dirname(_SCRIPT_DIR)


def main():
  parser = argparse.ArgumentParser(description='build http proxy')
  parser.add_argument('--output', help='proxy filename', required=True)
  parser.add_argument(
      '--cache-dir', help='gocache directory name', required=True)
  parser.add_argument(
      '--goarch', help='go architecture for which to compile', required=True)
  args = parser.parse_args()

  gopath = os.path.join(_TOP_DIR, 'third_party', 'go', 'bin', 'go')
  env = os.environ.copy()
  for key in ('GOROOT', 'GOPATH'):
    env.pop(key, None)
  env['CGO_ENABLED'] = '0'
  env['GOCACHE'] = args.cache_dir
  env['GOARCH'] = args.goarch
  args = [gopath, 'build', '-o', args.output]
  args.append(os.path.join(_SCRIPT_DIR, 'proxy', 'proxy.go'))
  if os.name == 'posix':
    args.append(os.path.join(_SCRIPT_DIR, 'proxy', 'rlimit_posix.go'))
  else:
    args.append(os.path.join(_SCRIPT_DIR, 'proxy', 'rlimit_windows.go'))
  subprocess.check_call(args, env=env)
  return 0


if __name__ == '__main__':
  sys.exit(main())
