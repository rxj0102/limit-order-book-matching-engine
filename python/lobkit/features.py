"""Market microstructure features on plain numpy arrays.

Every function takes arrays and returns arrays; none touches the engine. Prices and sizes can
be in any consistent unit. Each docstring gives the formula and the source. The tests in
python/tests/test_features.py check each function against values worked out by hand on tiny
inputs.

Conventions: a trade sign is +1 for a buyer initiated trade and -1 for a seller initiated one.
Where a feature is undefined (empty book side, no mid price at the horizon) the result is NaN
rather than a guess.
"""
from __future__ import annotations

import numpy as np

__all__ = ["order_flow_imbalance", "queue_imbalance", "microprice", "realized_spread", "vpin"]


def _f(a):
    return np.asarray(a, dtype=np.float64)


def order_flow_imbalance(bid_px, bid_sz, ask_px, ask_sz):
    """Order flow imbalance per book update, from best bid and ask snapshots.

    For consecutive snapshots n-1 and n (Cont, Kukanov and Stoikov, "The Price Impact of Order
    Book Events", Journal of Financial Econometrics 12(1), 2014, eq. 3):

        e_n =  1[Pb_n >= Pb_{n-1}] * qb_n  -  1[Pb_n <= Pb_{n-1}] * qb_{n-1}
             - 1[Pa_n <= Pa_{n-1}] * qa_n  +  1[Pa_n >= Pa_{n-1}] * qa_{n-1}

    where Pb, Pa are the best bid and ask prices and qb, qa the sizes there. Positive values
    mean net buying pressure. The result has one element fewer than the input; sum a window of
    it for the OFI over that window. Sizes at an empty side must be given as 0.
    """
    pb, qb, pa, qa = _f(bid_px), _f(bid_sz), _f(ask_px), _f(ask_sz)
    if not (pb.shape == qb.shape == pa.shape == qa.shape and pb.ndim == 1):
        raise ValueError("all four inputs must be one dimensional and equally long")
    pb0, pb1, qb0, qb1 = pb[:-1], pb[1:], qb[:-1], qb[1:]
    pa0, pa1, qa0, qa1 = pa[:-1], pa[1:], qa[:-1], qa[1:]
    return ((pb1 >= pb0) * qb1 - (pb1 <= pb0) * qb0
            - (pa1 <= pa0) * qa1 + (pa1 >= pa0) * qa0)


def queue_imbalance(bid_sz, ask_sz):
    """Top of book queue imbalance, (qb - qa) / (qb + qa), in [-1, 1].

    Positive when more size is queued on the bid. NaN where both sides are empty. It is
    the imbalance used as a state variable in Stoikov, "The Micro-Price: A High Frequency
    Estimator of Future Prices", Quantitative Finance 18(12), 2018.
    """
    qb, qa = _f(bid_sz), _f(ask_sz)
    tot = qb + qa
    with np.errstate(invalid="ignore", divide="ignore"):
        return np.where(tot > 0, (qb - qa) / tot, np.nan)


def microprice(bid_px, bid_sz, ask_px, ask_sz):
    """Size weighted mid price, (Pa * qb + Pb * qa) / (qb + qa).

    The first order version of the micro-price in Stoikov (2018): when the bid queue is larger
    the price is pulled toward the ask, because the ask is more likely to be consumed next.
    Equals the mid when qb == qa. NaN where both sizes are zero.
    """
    pb, qb, pa, qa = _f(bid_px), _f(bid_sz), _f(ask_px), _f(ask_sz)
    tot = qb + qa
    with np.errstate(invalid="ignore", divide="ignore"):
        return np.where(tot > 0, (pa * qb + pb * qa) / tot, np.nan)


def realized_spread(trade_ts, trade_px, trade_sign, mid_ts, mid_px, horizon):
    """Realized spread of each trade at a stated horizon, in price units.

    For a trade i at time t_i with price p_i and sign s_i, and the mid price m(t) observed at
    the first sample at or after t_i + horizon:

        RS_i = 2 * s_i * (p_i - m(t_i + horizon))

    A positive value is what the liquidity provider on the other side kept after the price had
    time to move against them; effective spread minus price impact (Huang and Stoll, "Dealer
    versus auction markets: a paired comparison of execution costs on NASDAQ and the NYSE",
    Journal of Financial Economics 41(3), 1996; Bessembinder, "Issues in assessing trade
    execution costs", Journal of Financial Markets 6(3), 2003). `horizon` is in the same unit as
    the timestamps and is not optional: the number means nothing without it. Trades with no mid
    sample at or after t_i + horizon get NaN. `mid_ts` must be sorted ascending.
    """
    if horizon < 0:
        raise ValueError("horizon must be non-negative")
    tt, tp, ts = _f(trade_ts), _f(trade_px), _f(trade_sign)
    mt, mp = _f(mid_ts), _f(mid_px)
    if mt.size and np.any(np.diff(mt) < 0):
        raise ValueError("mid_ts must be sorted ascending")
    idx = np.searchsorted(mt, tt + horizon, side="left")
    ok = idx < mt.size
    out = np.full(tt.shape, np.nan)
    out[ok] = 2.0 * ts[ok] * (tp[ok] - mp[idx[ok]])
    return out


def vpin(volume, sign, bucket_volume, window=50):
    """Volume synchronized probability of informed trading, simple version.

    Trades are cut into buckets of exactly `bucket_volume` units of volume, in time order. A
    trade that straddles a bucket boundary is split in proportion, keeping its sign. For
    bucket j with buy volume B_j and sell volume S_j:

        imbalance_j = |B_j - S_j| / bucket_volume
        VPIN_j      = mean(imbalance over the last `window` buckets ending at j)

    This follows Easley, Lopez de Prado and O'Hara, "Flow Toxicity and Liquidity in a
    High-Frequency World", Review of Financial Studies 25(5), 2012, with one simplification:
    the original classifies volume as buy or sell with bulk volume classification from price
    changes, whereas here the caller supplies the sign of each trade (the simulator and LOBSTER
    both know the aggressor). The trailing incomplete bucket is dropped. Typical practice sets
    the bucket to about 1/50 of average daily volume; there is no default here because the
    right value depends on the instrument.

    Returns (imbalance, vpin). `vpin` has NaN for the first window-1 buckets.
    """
    if bucket_volume <= 0:
        raise ValueError("bucket_volume must be positive")
    if window < 1:
        raise ValueError("window must be at least 1")
    v, s = _f(volume), _f(sign)
    if v.shape != s.shape or v.ndim != 1:
        raise ValueError("volume and sign must be one dimensional and equally long")
    if np.any(v < 0):
        raise ValueError("volume must be non-negative")
    cum = np.concatenate(([0.0], np.cumsum(v)))
    cum_buy = np.concatenate(([0.0], np.cumsum(np.where(s > 0, v, 0.0))))
    nb = int(cum[-1] // bucket_volume)
    if nb == 0:
        return np.empty(0), np.empty(0)
    edges = np.arange(nb + 1) * float(bucket_volume)
    # Cumulative buy volume is piecewise linear in cumulative volume, so interpolation splits a
    # straddling trade proportionally.
    buy_at = np.interp(edges, cum, cum_buy)
    buy = np.diff(buy_at)
    sell = float(bucket_volume) - buy
    imb = np.abs(buy - sell) / float(bucket_volume)
    out = np.full(nb, np.nan)
    if nb >= window:
        c = np.concatenate(([0.0], np.cumsum(imb)))
        out[window - 1:] = (c[window:] - c[:-window]) / window
    return imb, out
