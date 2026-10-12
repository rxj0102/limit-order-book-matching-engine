"""lobkit: Python front end for the lobkit C++ matching engine and simulator.

All bundled data generators produce SYNTHETIC data. Nothing here is evidence that a strategy
is profitable.
"""
from . import _core
from ._core import *  # noqa: F401,F403
from . import features  # noqa: F401
from .helpers import book_config_for, strategy_params  # noqa: F401
