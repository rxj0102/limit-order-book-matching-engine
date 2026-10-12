"""Small conveniences that mirror what the lobkit_sim_run command line tool does, so a Python
run and a command line run with the same arguments are the same run."""
from __future__ import annotations

from . import _core


def book_config_for(messages, tick=100, num_levels=65536, max_orders=1 << 20):
    """BookConfig whose price band is centred on the prices in `messages` (FEED_DTYPE array),
    chosen by the same rule as the command line tool."""
    rng = _core.price_range(messages)
    base = _core.suggest_base_price(rng[0], rng[1], tick, num_levels) if rng else 0
    return _core.BookConfig(tick_size=tick, base_price=base, num_levels=num_levels, max_orders=max_orders)


def strategy_params(book, **kw):
    """ASParams with the quoting price limits set to the inside of the book band (as the command
    line tool does), plus any keyword overrides. `tick` is taken from the book unless given."""
    kw.setdefault("tick", book.tick_size)
    kw.setdefault("min_price", book.base_price + book.tick_size)
    kw.setdefault("max_price", book.base_price + (book.num_levels - 2) * book.tick_size)
    return _core.ASParams(**kw)
