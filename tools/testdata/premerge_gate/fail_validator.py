#!/usr/bin/env python3
"""Deterministic failing validator fixture for run_premerge_gate.py self-test."""

import sys

print("fixture validator failed", file=sys.stderr)
raise SystemExit(7)
