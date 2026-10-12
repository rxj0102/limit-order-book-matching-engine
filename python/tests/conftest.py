import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Tests run against the built package: PYTHONPATH=<build>/python (ctest sets it). Fall back to
# the default preset directory so plain `pytest` works after `cmake --build build/python`.
try:
    import lobkit  # noqa: F401
except ImportError:
    sys.path.insert(0, str(ROOT / "build" / "python" / "python"))
