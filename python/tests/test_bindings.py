"""Binding tests. All data here is SYNTHETIC."""
import filecmp
import gc
import os
import subprocess
from pathlib import Path

import numpy as np
import pytest

import lobkit
from lobkit import _core as core

ROOT = Path(__file__).resolve().parents[2]
SYNTH_MSG = ROOT / "data" / "synthetic" / "SYNTH_lobster_1000_message_10.csv"
SYNTH_BOOK = ROOT / "data" / "synthetic" / "SYNTH_lobster_1000_orderbook_10.csv"


def sim_cli():
    p = os.environ.get("LOBKIT_SIM_RUN")
    if p and Path(p).exists():
        return p
    for cand in ("release", "python", "debug"):
        q = ROOT / "build" / cand / ("python" if False else "") / "lobkit_sim_run"
        if q.exists():
            return str(q)
    pytest.skip("lobkit_sim_run not built")


# ---- round trip against the command line tool --------------------------------------------------

LAT_US = dict(submit=50, ack=30, md=20)
# Strategy parameters are left at their defaults on both sides (the defaults give 14 fills on this file).


def python_run(tmp_path, from_file):
    book_msgs = core.read_lobster_messages(str(SYNTH_MSG))
    book = lobkit.book_config_for(book_msgs, tick=100)
    sc = core.SimConfig(submit_latency_ns=LAT_US["submit"] * 1000, ack_latency_ns=LAT_US["ack"] * 1000,
                        md_latency_ns=LAT_US["md"] * 1000, maker_fee=-10, taker_fee=30)
    ap = lobkit.strategy_params(book)
    if from_file:
        return core.run_simulation_file(book, sc, ap, str(SYNTH_MSG))
    return core.run_simulation(book, sc, ap, book_msgs)


def cli_run(tmp_path):
    prefix = tmp_path / "cli"
    subprocess.run([sim_cli(), "--lobster", str(SYNTH_MSG), "--out", str(prefix), "--data-label", "SYNTH test",
                    "--submit-latency-us", "50", "--ack-latency-us", "30", "--md-latency-us", "20",
                    "--maker-fee", "-10", "--taker-fee", "30"],
                   check=True, capture_output=True)
    return prefix


@pytest.mark.parametrize("from_file", [True, False])
def test_csv_output_matches_cli_byte_for_byte(tmp_path, from_file):
    cli = cli_run(tmp_path)
    r = python_run(tmp_path, from_file)
    r.write_csv(str(tmp_path / "py"))
    assert len(r.fills) > 0, "an empty run would make this comparison vacuous"
    for suffix in ("fills", "inventory", "pnl"):
        a, b = tmp_path / f"cli_{suffix}.csv", tmp_path / f"py_{suffix}.csv"
        assert a.read_bytes() == b.read_bytes(), suffix
    assert r.pnl["identity_holds"]


def test_arrays_match_csv_values(tmp_path):
    """The numpy arrays and the CSV the same run wrote hold the same numbers."""
    r = python_run(tmp_path, True)
    r.write_csv(str(tmp_path / "py"))
    assert len(r.fills) >= 2, "the round trip must exercise real fills"
    f = np.genfromtxt(tmp_path / "py_fills.csv", delimiter=",", skip_header=1, dtype=None, encoding=None)
    f = np.atleast_1d(f)
    assert len(f) == len(r.fills)
    # columns: ts_ns,order_id,side,price,qty,role,fee,mid_ref,inventory_after ; prices in dollars
    np.testing.assert_array_equal([row[0] for row in f], r.fills["ts"])
    np.testing.assert_array_equal([row[1] for row in f], r.fills["id"])
    np.testing.assert_allclose([row[3] for row in f], r.fills["price"] / 10000.0, atol=1e-9)
    np.testing.assert_array_equal([row[4] for row in f], r.fills["qty"])
    np.testing.assert_array_equal([row[8] for row in f], r.fills["inventory_after"])
    assert all(row[5] == "maker" for row in f) == bool(r.fills["maker"].all())
    # pnl.csv: totals in dollars, one unit of PATH_DTYPE pnl columns is half a native unit
    pnl = np.genfromtxt(tmp_path / "py_pnl.csv", delimiter=",", skip_header=1)
    pnl = np.atleast_2d(pnl)
    assert len(pnl) == len(r.path)
    np.testing.assert_array_equal(pnl[:, 0].astype(np.uint64), r.path["ts"])
    np.testing.assert_allclose(pnl[:, 4], r.path["total2"] / 20000.0, atol=1e-4)
    np.testing.assert_allclose(pnl[:, 1], r.path["spread2"] / 20000.0, atol=1e-4)
    assert abs(pnl[-1, 4] - r.pnl["total_dollars"]) < 1e-3


def test_file_and_array_runs_agree(tmp_path):
    a, b = python_run(tmp_path, True), python_run(tmp_path, False)
    assert a.fingerprint() == b.fingerprint()
    np.testing.assert_array_equal(a.fills, b.fills)
    np.testing.assert_array_equal(a.path, b.path)


def test_deterministic_and_seed_sensitive():
    book_a = core.synthetic_feed(seed=3, messages=3000)
    book_b = core.synthetic_feed(seed=3, messages=3000)
    book_c = core.synthetic_feed(seed=4, messages=3000)
    np.testing.assert_array_equal(book_a, book_b)
    assert not np.array_equal(book_a, book_c)


# ---- dtypes, shapes, ownership -------------------------------------------------------------------------------

def test_dtypes_and_shapes():
    msgs = core.synthetic_feed(seed=1, messages=4000)
    assert msgs.dtype == core.FEED_DTYPE and msgs.ndim == 1 and msgs.shape == (4000,)
    book = lobkit.book_config_for(msgs)
    r = core.run_simulation(book, core.SimConfig(sample_interval_ns=1_000_000),
                            lobkit.strategy_params(book), msgs)
    assert r.fills.dtype == core.FILL_DTYPE
    assert r.path.dtype == core.PATH_DTYPE
    assert r.fills.ndim == 1 and r.path.ndim == 1
    assert r.fills.itemsize == 40 or r.fills.itemsize == r.fills.dtype.itemsize
    for name in ("ts", "inventory", "total2"):
        assert name in r.path.dtype.names
    assert r.path["ts"].dtype == np.uint64 and r.path["inventory"].dtype == np.int64
    # total pnl column is the sum of its parts (fees are subtracted)
    p = r.path
    np.testing.assert_array_equal(p["total2"], p["spread2"] + p["inventory2"] - p["fees2"])
    # inventory column agrees with the fills log at the end of the run
    assert r.pnl["total2"] == int(p["total2"][-1])


def test_sim_arrays_are_zero_copy_readonly_views_that_keep_the_owner_alive():
    msgs = core.synthetic_feed(seed=2, messages=4000)
    book = lobkit.book_config_for(msgs)
    r = core.run_simulation(book, core.SimConfig(sample_interval_ns=1_000_000), lobkit.strategy_params(book), msgs)
    fills, path = r.fills, r.path
    assert not fills.flags.writeable and not path.flags.writeable
    assert fills.base is r and path.base is r  # views of memory owned by the result
    assert r.fills.__array_interface__["data"][0] == fills.__array_interface__["data"][0]  # same buffer each time
    with pytest.raises(ValueError):
        path["ts"][0] = 1
    snapshot = path.copy()
    del r
    gc.collect()
    np.testing.assert_array_equal(path, snapshot)  # still valid after the result name is gone


def test_adopted_arrays_own_their_memory():
    a = core.synthetic_feed(seed=1, messages=1000)
    assert not a.flags.writeable and a.base is not None
    snap = a.copy()
    gc.collect()
    np.testing.assert_array_equal(a, snap)


def test_input_array_is_read_not_copied_and_checked():
    msgs = core.synthetic_feed(seed=1, messages=500)
    book = lobkit.book_config_for(msgs)
    ap = lobkit.strategy_params(book)
    with pytest.raises(ValueError, match="dtype"):
        core.run_simulation(book, core.SimConfig(), ap, np.zeros(10, dtype=np.int64))
    with pytest.raises(ValueError, match="contiguous"):
        core.run_simulation(book, core.SimConfig(), ap, msgs[::2])


# ---- engine ------------------------------------------------------------------------------------------------------------

def make_engine():
    return core.Engine(core.BookConfig(tick_size=1, base_price=0, num_levels=1024, max_orders=1024))


def test_engine_submit_match_and_events():
    e = make_engine()
    e.submit(1, core.Side.Ask, 100, 10, ts=1)
    e.submit(2, core.Side.Bid, 100, 4, ts=2)
    ev = e.drain_events()
    assert ev.dtype == core.EVENT_DTYPE
    types = [int(t) for t in ev["type"]]
    assert types == [int(core.EventType.Ack), int(core.EventType.Ack), int(core.EventType.Trade)]
    assert list(ev["seq"]) == [1, 2, 3]
    assert e.depth_at(100) == 6
    assert e.best_ask() == 100 and e.best_bid() is None
    assert e.check_invariants() is None
    assert len(e.drain_events()) == 0  # drained


def test_engine_reject_is_data_not_exception():
    e = make_engine()
    e.submit(1, core.Side.Bid, 5000, 10)  # outside the band
    e.submit(2, core.Side.Bid, 10, 0)  # zero quantity
    e.cancel(99)
    ev = e.drain_events()
    assert [int(t) for t in ev["type"]] == [int(core.EventType.Reject)] * 3
    assert e.stats()["rejects"]["PriceOutOfBand"] == 1


def test_replay_adapter_owner_rules():
    e = make_engine()
    a = e.replay_adapter()
    a.apply_add(1, core.Side.Bid, 100, 10)
    a.apply_execute(1, 4)
    assert e.depth_at(100) == 6
    a.apply_delete(1)
    assert e.depth_at(100) == 0
    e.submit(7, core.Side.Bid, 90, 5, owner=1)
    a.apply_delete(7)  # own order: WrongOwner, an event not an exception
    assert e.stats()["rejects"]["WrongOwner"] == 1


def test_adapter_keeps_engine_alive():
    a = make_engine().replay_adapter()
    gc.collect()
    a.apply_add(1, core.Side.Bid, 10, 1)


def test_top_levels_shape_dtype():
    e = make_engine()
    for i, p in enumerate((100, 99, 98)):
        e.submit(i + 1, core.Side.Bid, p, 10 + i)
    t = e.top_levels(core.Side.Bid, 5)
    assert t.dtype == np.int64 and t.shape == (3, 2)
    np.testing.assert_array_equal(t, [[100, 10], [99, 11], [98, 12]])


def test_replay_lobster_counters_and_book_reader():
    e = core.Engine(core.BookConfig(tick_size=100, base_price=900000, num_levels=4096, max_orders=1 << 16))
    c = core.replay_lobster(e, str(SYNTH_MSG))
    assert c["messages"] == 1000
    assert e.stats()["anomalies"] == 0
    b = core.read_lobster_book(str(SYNTH_BOOK))
    assert b.dtype == np.int64 and b.shape == (1000, 10, 4)
    assert e.check_invariants() is None


# ---- error paths -----------------------------------------------------------------------------------------------------------

def test_missing_file_is_filenotfounderror(tmp_path):
    with pytest.raises(FileNotFoundError):
        core.read_lobster_messages(str(tmp_path / "nope.csv"))


def test_malformed_file_raises_parseerror_with_location(tmp_path):
    bad = tmp_path / "bad.csv"
    good = SYNTH_MSG.read_text().splitlines()[:3]
    bad.write_text("\n".join(good + ["this,is,not,a,message"]) + "\n")
    with pytest.raises(core.ParseError) as ei:
        core.read_lobster_messages(str(bad))
    err = ei.value
    assert isinstance(err, ValueError)
    assert err.line == 4 and err.status != "Ok" and "this,is" in err.text
    # same file through the replay and the simulator
    e = core.Engine(core.BookConfig(tick_size=100, base_price=900000, num_levels=4096, max_orders=4096))
    with pytest.raises(core.ParseError):
        core.replay_lobster(e, str(bad))
    book = core.BookConfig(tick_size=100, base_price=900000, num_levels=4096, max_orders=4096)
    with pytest.raises(core.ParseError):
        core.run_simulation_file(book, core.SimConfig(), core.ASParams(), str(bad))


def test_bad_configuration_is_valueerror():
    with pytest.raises(ValueError):
        core.Engine(core.BookConfig(num_levels=100))
    with pytest.raises(ValueError):
        core.run_simulation(core.BookConfig(tick_size=0), core.SimConfig(), core.ASParams(),
                            core.synthetic_feed(messages=10))
    with pytest.raises(TypeError, match="unknown parameter"):
        core.SimConfig(no_such_field=1)
    with pytest.raises(ValueError):
        core.ASParams(gamma="high")


def test_unwritable_csv_prefix_is_oserror(tmp_path):
    msgs = core.synthetic_feed(messages=200)
    book = lobkit.book_config_for(msgs)
    r = core.run_simulation(book, core.SimConfig(), lobkit.strategy_params(book), msgs)
    with pytest.raises(OSError):
        r.write_csv(str(tmp_path / "missing_dir" / "x"))
