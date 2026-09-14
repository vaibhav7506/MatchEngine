"""Binding fidelity: native values and native exception behavior, through Python."""
import gc
import pytest

from matchengine import Engine, Order, OrderStatus as Status, OrderType as Type, Side


def order(id, side=Side.SELL, price=100, quantity=5, trader=None, **kwargs):
    return Order(order_id=id, trader_id=id if trader is None else trader,
                 side=side, price=price, quantity=quantity, **kwargs)


@pytest.mark.parametrize("side", [Side.BUY, Side.SELL])
def test_exact_match_all_trade_fields_and_lifetime(side):
    e = Engine()
    assert e.get_top_of_book().best_bid is None
    assert e.get_top_of_book().best_ask is None
    assert e.add_order(order(1, side)) == []
    opposite = Side.SELL if side == Side.BUY else Side.BUY
    trades = e.add_order(order(2, opposite, timestamp=1))
    assert len(trades) == 1
    t = trades[0]
    assert (t.trade_id, t.price, t.quantity) == (1, 100, 5)
    assert (t.aggressor_order_id, t.resting_order_id, t.aggressor_side) == (2, 1, opposite)
    assert t.timestamp > 1  # C++ wall time; not caller historical time
    assert e.get_order_status(1) == e.get_order_status(2) == Status.FILLED
    del e, trades
    gc.collect()
    assert t.quantity == 5  # returned value owns storage independently of Engine


def test_partial_sweep_snapshots_and_optional_top():
    e = Engine()
    for id, price in ((3, 102), (1, 100), (2, 101)):
        e.add_order(order(id, price=price))
    trades = e.add_order(order(4, Side.BUY, 102, 12))
    assert [(t.resting_order_id, t.price, t.quantity) for t in trades] == [(1, 100, 5), (2, 101, 5), (3, 102, 2)]
    assert e.get_order_status(3) == Status.PARTIALLY_FILLED
    assert e.get_order_status(4) == Status.FILLED
    top = e.get_top_of_book()
    assert (top.best_bid, top.bid_qty, top.best_ask, top.ask_qty) == (None, 0, 102, 3)
    snap = e.get_book_snapshot(1)
    assert snap.bids == []
    assert [(l.price, l.quantity, l.order_count) for l in snap.asks] == [(102, 3, 1)]
    assert e.get_book_snapshot(0).asks == []
    e.cancel_order(3)
    assert snap.asks[0].quantity == 3  # snapshot stays a value
    assert e.get_top_of_book().best_ask is None


def test_fifo_ignores_reversed_timestamps_and_copies_submission():
    e = Engine()
    first = order(1, timestamp=200)
    e.add_order(first)
    first.price = 200
    e.add_order(order(2, timestamp=100))
    trades = e.add_order(order(3, Side.BUY, quantity=7))
    assert [(t.resting_order_id, t.quantity, t.price) for t in trades] == [(1, 5, 100), (2, 2, 100)]


def test_self_trade_skip_and_crossed_book():
    e = Engine()
    e.add_order(order(1, trader=0))
    e.add_order(order(2, trader=2))
    trades = e.add_order(order(3, Side.BUY, quantity=7, trader=0))
    assert [(t.resting_order_id, t.quantity) for t in trades] == [(2, 5)]
    assert e.get_order_status(1) == Status.NEW
    assert e.get_order_status(3) == Status.PARTIALLY_FILLED
    assert e.get_top_of_book().best_bid == e.get_top_of_book().best_ask == 100


def test_fok_rejection_does_not_mutate_book_statuses_or_trade_counter():
    e = Engine()
    e.add_order(order(1, quantity=100, trader=7))
    e.add_order(order(2, quantity=5, trader=8))
    e.add_order(order(3, price=102, quantity=100, trader=9))
    assert e.add_order(order(4, Side.BUY, quantity=6, trader=7, type=Type.FOK)) == []
    assert e.get_order_status(4) == Status.REJECTED
    assert e.get_order_status(2) == Status.NEW
    assert e.get_book_snapshot(1).asks[0].quantity == 105
    trades = e.add_order(order(5, Side.BUY, quantity=5, trader=7, type=Type.FOK))
    assert len(trades) == 1
    assert (trades[0].trade_id, trades[0].resting_order_id, trades[0].quantity) == (1, 2, 5)
    assert e.get_order_status(5) == Status.FILLED


@pytest.mark.parametrize("price", [99, 100])
def test_modify_always_loses_fifo(price):
    e = Engine()
    e.add_order(order(1, Side.BUY, price))
    e.add_order(order(2, Side.BUY))
    assert e.modify_order(1, 100, 7) == []
    trades = e.add_order(order(3, Side.SELL, quantity=12))
    assert [(t.resting_order_id, t.quantity) for t in trades] == [(2, 5), (1, 7)]


def test_modify_quantity_is_new_remaining_and_can_cross():
    e = Engine()
    e.add_order(order(1, Side.BUY, 99, 10))
    e.add_order(order(2, Side.SELL, 100, 5))
    trades = e.modify_order(1, 100, 7)
    assert trades[0].quantity == 5
    assert e.get_top_of_book().bid_qty == 2
    e.modify_order(1, 99, 4)
    assert e.get_top_of_book().bid_qty == 4


def test_exception_translation_and_cancel():
    e = Engine()
    e.add_order(order(1))
    with pytest.raises(ValueError):
        e.add_order(order(1))
    with pytest.raises(IndexError):
        e.get_order_status(999)
    with pytest.raises(IndexError):
        e.modify_order(999, 100, 5)
    with pytest.raises(ValueError):
        e.modify_order(1, 100, 0)
    with pytest.raises(ValueError):
        e.modify_order(1, float("nan"), 5)
    assert e.get_order_status(1) == Status.NEW
    assert e.cancel_order(1) is True
    assert e.cancel_order(1) is False
    assert e.cancel_order(999) is False
    with pytest.raises(ValueError):
        e.add_order(order(1))  # cancellation does not recycle the ID


@pytest.mark.parametrize("quantity,price", [(0, 100), (2**32, 100), (5, 0), (5, float("inf")), (5, float("nan"))])
def test_invalid_fresh_inputs_are_rejected_not_python_exceptions(quantity, price):
    e = Engine()
    assert e.add_order(order(1, quantity=quantity, price=price)) == []
    assert e.get_order_status(1) == Status.REJECTED


def test_unsigned_ranges_and_metadata_fields():
    o = Order(order_id=2**64 - 1, trader_id=2**64 - 1, quantity=2**32 - 1,
              price=100, timestamp=2**64 - 1, remaining_quantity=3, status=Status.CANCELLED)
    assert (o.order_id, o.trader_id, o.timestamp) == (2**64 - 1,) * 3
    assert o.remaining_quantity == 3 and o.status == Status.CANCELLED
    e = Engine()
    e.add_order(o)
    assert e.get_top_of_book().bid_qty == 2**32 - 1
    assert e.get_order_status(o.order_id) == Status.NEW
    for field in ("order_id", "trader_id", "quantity", "timestamp"):
        with pytest.raises(TypeError):
            setattr(o, field, -1)
        with pytest.raises(TypeError):
            setattr(o, field, 2**64)


@pytest.mark.parametrize("type", [Type.IOC, Type.MARKET])
def test_market_and_ioc_unfilled_remainders_cancel(type):
    e = Engine()
    e.add_order(order(1))
    trades = e.add_order(order(2, Side.BUY, quantity=10, type=type))
    assert trades[0].quantity == 5
    assert e.get_order_status(2) == Status.CANCELLED
    assert e.get_top_of_book().best_bid is None
