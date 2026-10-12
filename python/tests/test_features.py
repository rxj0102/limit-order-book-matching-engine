"""Features against values worked out by hand on tiny inputs (working shown in comments)."""
import numpy as np
import pytest

from lobkit import features as F

nan = np.nan


def test_ofi_hand_computed():
    # bid_px 100,100,101,100 / bid_sz 10,15,5,7 / ask_px 102,102,102,101 / ask_sz 8,8,9,4
    # step1: bid same -> 15-10 = +5; ask same -> -8+8 = 0                      => 5
    # step2: bid up   -> +5 (no old term); ask same -> -9+8 = -1               => 4
    # step3: bid down -> -5 (old bid size); ask down -> -4 (new ask size)      => -9
    got = F.order_flow_imbalance([100, 100, 101, 100], [10, 15, 5, 7], [102, 102, 102, 101], [8, 8, 9, 4])
    np.testing.assert_array_equal(got, [5, 4, -9])


def test_ofi_ask_up_removes_old_ask_size():
    # ask moves up 102 -> 103: the old ask queue is gone, which is buying pressure: +qa_old = +8
    got = F.order_flow_imbalance([100, 100], [10, 10], [102, 103], [8, 6])
    np.testing.assert_array_equal(got, [8])


def test_ofi_validates_shapes():
    with pytest.raises(ValueError):
        F.order_flow_imbalance([1, 2], [1], [1, 2], [1, 2])


def test_queue_imbalance():
    # (30-10)/40 = 0.5 ; (0-10)/10 = -1 ; empty book -> nan
    got = F.queue_imbalance([30, 0, 0], [10, 10, 0])
    np.testing.assert_allclose(got, [0.5, -1.0, nan])


def test_microprice():
    # (102*30 + 100*10)/40 = 4060/40 = 101.5 ; equal sizes -> plain mid 101 ; empty -> nan
    got = F.microprice([100, 100, 100], [30, 5, 0], [102, 102, 102], [10, 5, 0])
    np.testing.assert_allclose(got, [101.5, 101.0, nan])


def test_realized_spread_horizon_5():
    ts, px, sg = [0, 10, 20], [101, 99, 100.5], [1, -1, 1]
    mts, mp = [0, 5, 10, 15, 20, 25], [100, 100, 100.5, 101, 100.2, 99]
    # trade0 -> mid at t>=5 is 100 : 2*(+1)*(101-100)     = 2
    # trade1 -> mid at t>=15 is 101: 2*(-1)*(99-101)      = 4
    # trade2 -> mid at t>=25 is 99 : 2*(+1)*(100.5-99)    = 3
    np.testing.assert_allclose(F.realized_spread(ts, px, sg, mts, mp, 5), [2, 4, 3])


def test_realized_spread_horizon_10_and_missing_mid():
    ts, px, sg = [0, 10, 20], [101, 99, 100.5], [1, -1, 1]
    mts, mp = [0, 5, 10, 15, 20, 25], [100, 100, 100.5, 101, 100.2, 99]
    # trade0 -> t>=10: 100.5 -> 2*0.5 = 1 ; trade1 -> t>=20: 100.2 -> -2*(99-100.2) = 2.4 ; trade2 -> none
    np.testing.assert_allclose(F.realized_spread(ts, px, sg, mts, mp, 10), [1, 2.4, nan])


def test_realized_spread_rejects_bad_input():
    with pytest.raises(ValueError):
        F.realized_spread([0], [1], [1], [0], [1], -1)
    with pytest.raises(ValueError):
        F.realized_spread([0], [1], [1], [5, 0], [1, 1], 1)


def test_vpin_buckets_and_window():
    # volumes 4,6,5,5 signs +,-,+,-, bucket 5. Buckets: [4 buy,1 sell], [5 sell], [5 buy], [5 sell]
    # imbalance = |B-S|/5 = 0.6, 1, 1, 1 ; window 2 -> nan, 0.8, 1, 1
    imb, v = F.vpin([4, 6, 5, 5], [1, -1, 1, -1], 5, window=2)
    np.testing.assert_allclose(imb, [0.6, 1, 1, 1])
    np.testing.assert_allclose(v, [nan, 0.8, 1, 1])


def test_vpin_split_trade_and_dropped_tail():
    # trades 3 buy, 4 sell, bucket 5: first bucket = 3 buy + 2 of the sell -> |3-2|/5 = 0.2 ; the
    # remaining 2 units are an incomplete bucket and are dropped
    imb, v = F.vpin([3, 4], [1, -1], 5, window=1)
    np.testing.assert_allclose(imb, [0.2])
    np.testing.assert_allclose(v, [0.2])


def test_vpin_no_complete_bucket_and_validation():
    imb, v = F.vpin([1, 1], [1, -1], 5)
    assert imb.size == 0 and v.size == 0
    with pytest.raises(ValueError):
        F.vpin([1], [1], 0)
    with pytest.raises(ValueError):
        F.vpin([-1], [1], 1)
