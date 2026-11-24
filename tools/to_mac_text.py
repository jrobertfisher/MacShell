#!/usr/bin/env python3
"""Copies a text file with Mac (CR) line endings.  Usage: to_mac_text.py in out"""
import sys
data = open(sys.argv[1], 'rb').read().replace(b'\r\n', b'\n').replace(b'\n', b'\r')
open(sys.argv[2], 'wb').write(data)
