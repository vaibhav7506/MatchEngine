from dataclasses import replace
import json

import pytest

from matchengine import OrderStatus, OrderType, Side
from matchengine.data import Bar, DAY_NS, date_ns, load_binance, validate_bars
from matchengine.replay import Broker, OrderIDAllocator, Replay, ReplayConfig
from matchengine.strategies import MeanReversion, MovingAverageCrossover


def candles(prices, volume=1000):
    start = date_ns("2023-01-01")
    return tuple(Bar(start + i * DAY_NS, start + (i + 1) * DAY_NS,
                     p, p * 1.01, p * 0.99, p, volume) for i, p in enumerate(prices))


def test_validator_reports_anomalies_and_never_silently_drops(caplog):
    a, b, c, d = candles([100, 100, 150, 100])
    invalid = replace(d, low=200)
    result = validate_bars([c, a, a, invalid])
    codes = {i.code for i in result.issues}
    assert {"non_monotonic", "duplicate", "invalid_bar", "missing_bars", "outlier_gap"} <= codes
    assert result.bars == (a, c)
    assert result.skipped_rows == 2
    assert "Duplicate timestamp" in caplog.text


def test_validation_edge_coverage_and_known_event():
    a, b, c = candles([100, 150, 150])
    result = validate_bars([b], start_ns=a.timestamp, end_ns=c.end_timestamp)
    assert {i.code for i in result.issues} == {"missing_start", "missing_end"}
    result = validate_bars([a, b], known_events=frozenset({b.timestamp}))
    assert not result.issues
    with pytest.raises(ValueError, match="No valid"):
        validate_bars([replace(a, volume=float("nan"))])


def test_raw_data_parse_error_is_explicit(tmp_path):
    path = tmp_path / "bad.json"
    path.write_text(json.dumps([[1, "not-a-price"]]))
    with pytest.raises(ValueError, match="Malformed Binance row 0"):
        load_binance(path)


def test_id_allocator_never_recycles_and_detects_exhaustion():
    ids = OrderIDAllocator()
    assert [ids.allocate() for _ in range(3)] == [1, 2, 3]
    ids._next = 2**64 - 1
    assert ids.allocate() == 2**64 - 1
    with pytest.raises(OverflowError):
        ids.allocate()


def test_fees_slippage_partial_fills_and_reconciliation():
    broker = Broker(ReplayConfig(initial_cash=1000, unit_size=1, fee_bps=10))
    broker.submit(trader_id=2, side=Side.BUY, quantity=10, timestamp=1, type=OrderType.LIMIT, price=99)
    broker.submit(trader_id=2, side=Side.SELL, quantity=2, timestamp=1, type=OrderType.LIMIT, price=101)
    id, trades = broker.submit(trader_id=1, side=Side.BUY, quantity=5, timestamp=2)
    assert len(trades) == 1 and trades[0].quantity == 2
    assert broker.status(id) == OrderStatus.CANCELLED
    fill = broker.portfolio.fills[0]
    assert fill.trade is broker.events[0].trade
    assert fill.expected_mid == 100
    assert fill.slippage == fill.adverse_slippage == 1
    assert fill.fee == pytest.approx(0.202)
    assert broker.portfolio.cash == pytest.approx(797.798)
    broker.reconcile(2)
    with pytest.raises(AssertionError):
        broker.reconcile(5)  # intended order quantity must never masquerade as a fill


def test_zero_fills_do_not_change_position_and_skip_counts_are_observed():
    broker = Broker(ReplayConfig())
    own, _ = broker.submit(trader_id=1, side=Side.SELL, quantity=10, timestamp=1,
                          type=OrderType.LIMIT, price=100)
    rejected, trades = broker.submit(trader_id=1, side=Side.BUY, quantity=5, timestamp=2,
                                    type=OrderType.FOK, price=100)
    assert trades == ()
    assert broker.status(rejected) == OrderStatus.REJECTED
    assert broker.fok_rejections == 1
    assert broker.self_trade_skips == 0  # failed preflight is not a matching pass
    _, trades = broker.submit(trader_id=1, side=Side.BUY, quantity=5, timestamp=3)
    assert trades == () and broker.self_trade_skips == 1
    assert broker.portfolio.position == 0 and broker.portfolio.fees == 0
    broker.reconcile(0)
    assert broker.cancel(own)
    new_id, _ = broker.submit(trader_id=1, side=Side.BUY, quantity=1, timestamp=4)
    assert new_id > rejected > own


def test_passive_strategy_fill_side_and_missing_mid():
    broker = Broker(ReplayConfig(unit_size=1, fee_bps=0))
    broker.submit(trader_id=1, side=Side.BUY, quantity=5, timestamp=1,
                  type=OrderType.LIMIT, price=100)
    broker.submit(trader_id=2, side=Side.SELL, quantity=2, timestamp=2)
    fill = broker.portfolio.fills[0]
    assert fill.side == Side.BUY
    assert fill.trade.aggressor_side == Side.SELL
    assert fill.expected_mid is None and fill.slippage is None
    broker.reconcile(2)


@pytest.mark.parametrize("factory,prices", [
    (lambda: MovingAverageCrossover(2, 3, 5), [3, 2, 1, 2, 3, 2, 1]),
    (lambda: MeanReversion(3, 1, 5), [10, 10, 7, 10]),
])
def test_each_strategy_submits_real_buy_and_sell_orders(factory, prices):
    strategy = factory()
    result = Replay().run(candles(prices), strategy)
    assert [f.side for f in result.portfolio.fills] == [Side.BUY, Side.SELL]
    assert [f.trade.quantity for f in result.portfolio.fills] == [5, 5]
    assert strategy.position == result.portfolio.position == 0
    assert len(result.events) == 4 * len(prices) + 2
    assert result.portfolio.fees > 0


def test_no_fill_strategy_signals_never_create_positions():
    strategy = MeanReversion(3, 1, 5)
    result = Replay().run(candles([10, 10, 7, 10], volume=0), strategy)
    assert result.portfolio.fills == []
    assert strategy.position == 0
    assert result.ending_pnl == 0


def test_callbacks_expose_only_prefix_and_prefix_execution_is_future_independent():
    class Observer(MeanReversion):
        def __init__(self):
            super().__init__(3, 1, 5)
            self.seen = []

        def on_bar(self, bar, context):
            assert isinstance(context.history, tuple)
            assert context.history[-1] == bar
            assert all(b.end_timestamp <= bar.end_timestamp for b in context.history)
            self.seen.append(len(context.history))
            super().on_bar(bar, context)

    a, b = Observer(), Observer()
    left = Replay().run(candles([10, 10, 7, 10, 50]), a)
    right = Replay().run(candles([10, 10, 7, 10, 1]), b)
    assert a.seen == b.seen == [1, 2, 3, 4, 5]
    assert left.equity_curve[:4] == right.equity_curve[:4]
    left_prefix = [(e.trade.trade_id, e.trade.price, e.trade.quantity, e.simulation_timestamp)
                   for e in left.events if e.simulation_timestamp < candles([1] * 5)[4].timestamp]
    right_prefix = [(e.trade.trade_id, e.trade.price, e.trade.quantity, e.simulation_timestamp)
                    for e in right.events if e.simulation_timestamp < candles([1] * 5)[4].timestamp]
    assert left_prefix == right_prefix


def test_replay_rejects_bad_ordering_and_does_not_reuse_engines():
    bars = candles([10, 10, 7, 10])
    replay = Replay()
    with pytest.raises(ValueError, match="ordered"):
        replay.run(tuple(reversed(bars)), MeanReversion())
    left = replay.run(bars, MeanReversion(3, 1, 5))
    right = replay.run(bars, MeanReversion(3, 1, 5))
    assert left.events[0].trade.trade_id == right.events[0].trade.trade_id == 1
    assert left.equity_curve == right.equity_curve
    assert all(a.simulation_timestamp <= b.simulation_timestamp for a, b in zip(left.events, left.events[1:]))


@pytest.mark.parametrize("kwargs", [{"fee_bps": -1}, {"unit_size": 0}, {"spread_bps": float("nan")}])
def test_invalid_execution_config(kwargs):
    with pytest.raises(ValueError):
        ReplayConfig(**kwargs)
