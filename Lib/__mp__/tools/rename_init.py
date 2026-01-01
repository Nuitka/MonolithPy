#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import sys, os

sys.path = [os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")),
            os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "site-packages"))]

import time

import __mp__

def main():
    module_file = sys.argv[1]
    stamp_file = None
    if len(sys.argv) > 2:
        stamp_file = sys.argv[2]
    __mp__.rename_init_symbol_in_file(module_file)
    if stamp_file:
        with open(stamp_file, 'w') as f:
            f.write(str(time.time()))

if __name__ == "__main__":
    main()
