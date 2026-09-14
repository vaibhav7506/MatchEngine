"""Sequential synthetic liquidity and execution accounting through the native engine."""
from __future__ import annotations

from dataclasses import dataclass, field
import math
from typing import Protocol

from . import Engine, Order, OrderStatus, OrderType, Side, Trade
from .data import Bar

MAX_QTY = 2**32 - 1
STRATEGY_TRADER = 1
MAKER_TRADER = 2
TAPE_TRADER = 3


@dataclass(frozen=True)
class ReplayConfig:
    initial_cash: float = 10_000.0
    fee_bps: float = 5.0
    spread_bps: float = 2.0  # total bid/ask spread
    unit_size: float = 0.001  # one engine quantity unit = 0.001 BTC
    tick_size: float = 0.01
    volume_fraction: float = 0.0001  # modeled tape participation, not actual queue depth

    def __post_init__(self) -> None:
        for name in ("initial_cash", "unit_size", "tick_size", "volume_fraction"):
            value = getattr(self, name)
            if not math.isfinite(value) or value <= 0:
                raise ValueError(f"{name} must be finite and positive")
        for name in ("fee_bps", "spread_bps"):
            value = getattr(self, name)
            if not math.isfinite(value) or value < 0:
                raise ValueError(f"{name} must be finite and nonnegative")
        if self.spread_bps >= 10_000:
            raise ValueError("spread_bps must be below 10000")


@dataclass(frozen=True)
class TradeEvent:
    trade: Trade  # original C++ value, including unaltered wall-clock timestamp
    simulation_timestamp: int
    aggressor_trader_id: int
    resting_trader_id: int
    expected_mid: float | None


@dataclass(frozen=True)
class Fill:
    trade: Trade
    simulation_timestamp: int
    side: Side  # this portfolio's side, including passive fills
    expected_mid: float | None
    slippage: float | None  # actual price - pre-submission mid
    adverse_slippage: float | None  # positive is worse for this portfolio
    fee: float


@dataclass
class Portfolio:
    config: ReplayConfig
    cash: float = field(init=False)
    position: int = 0
    fees: float = 0.0
    fills: list[Fill] = field(default_factory=list)

    def __post_init__(self) -> None:
        self.cash = self.config.initial_cash

    def apply(self, event: TradeEvent, side: Side) -> Fill:
        trade = event.trade
        sign = 1 if side == Side.BUY else -1
        notional = trade.quantity * self.config.unit_size * trade.price
        fee = notional * self.config.fee_bps / 10_000
        self.cash -= sign * notional + fee
        self.position += sign * trade.quantity
        self.fees += fee
        slippage = None if event.expected_mid is None else trade.price - event.expected_mid
        fill = Fill(trade, event.simulation_timestamp, side, event.expected_mid, slippage,
                    None if slippage is None else sign * slippage, fee)
        self.fills.append(fill)
        return fill

    def equity(self, mark: float) -> float:
        return self.cash + self.position * self.config.unit_size * mark


class OrderIDAllocator:
    def __init__(self) -> None:
        self._next = 1

    def allocate(self) -> int:
        if self._next >= 2**64:
            raise OverflowError("Engine order ID space exhausted")
        result = self._next
        self._next += 1
        return result


@dataclass
class _Resting:
    order: Order
    remaining: int


class Broker:
    """Owns the engine/ID namespace; a mirror observes skips, never determines fills.

    All engine mutations must pass through this broker. Each fill and status is
    read from the native engine. The mirror only tracks live orders for diagnostics.
    """
    def __init__(self, config: ReplayConfig) -> None:
        self._engine = Engine()
        self._ids = OrderIDAllocator()
        self._owners: dict[int, int] = {}
        self._resting: dict[int, _Resting] = {}
        self.config = config
        self.portfolio = Portfolio(config)
        self.events: list[TradeEvent] = []
        self.self_trade_skips = 0
        self.fok_rejections = 0
        self.last_timestamp = 0
        self.on_fill = lambda fill: None

    def top(self):
        return self._engine.get_top_of_book()

    def status(self, order_id: int):
        return self._engine.get_order_status(order_id)

    def _skips(self, order: Order) -> int:
        remaining, skips = order.quantity, 0
        candidates = sorted((r for r in self._resting.values() if r.order.side != order.side),
                            key=lambda r: (r.order.price if order.side == Side.BUY else -r.order.price,
                                           r.order.order_id))
        for r in candidates:
            if not remaining:
                break
            if order.type != OrderType.MARKET and (
                    (order.side == Side.BUY and r.order.price > order.price)
                    or (order.side == Side.SELL and r.order.price < order.price)):
                break
            if r.order.trader_id == order.trader_id:
                skips += 1
            else:
                remaining -= min(remaining, r.remaining)
        return skips

    def submit(self, *, trader_id: int, side: Side, quantity: int, timestamp: int,
               type: OrderType = OrderType.MARKET, price: float = 0.0) -> tuple[int, tuple[Trade, ...]]:
        if timestamp <= 0 or timestamp < self.last_timestamp:
            raise ValueError("Replay submissions must have nondecreasing historical timestamps")
        self.last_timestamp = timestamp
        order_id = self._ids.allocate()
        order = Order(order_id=order_id, trader_id=trader_id, side=side, quantity=quantity,
                      timestamp=timestamp, type=type, price=price)
        top = self.top()
        # A crossed book is permitted by the engine, but is not a valid mid reference.
        mid = ((top.best_bid + top.best_ask) / 2 if top.best_bid is not None and
               top.best_ask is not None and top.best_bid <= top.best_ask else None)
        skips = self._skips(order)
        self._owners[order_id] = trader_id
        trades = tuple(self._engine.add_order(order))
        status = self.status(order_id)
        if type == OrderType.FOK and status == OrderStatus.REJECTED:
            self.fok_rejections += 1
        elif status != OrderStatus.REJECTED:
            self.self_trade_skips += skips
        for trade in trades:
            resting = self._resting[trade.resting_order_id]
            resting.remaining -= trade.quantity
            if resting.remaining == 0:
                del self._resting[trade.resting_order_id]
            event = TradeEvent(trade, timestamp, trader_id, self._owners[trade.resting_order_id], mid)
            self.events.append(event)
            if trader_id == STRATEGY_TRADER:
                self.on_fill(self.portfolio.apply(event, side))
            elif event.resting_trader_id == STRATEGY_TRADER:
                resting_side = Side.SELL if side == Side.BUY else Side.BUY
                self.on_fill(self.portfolio.apply(event, resting_side))
        if type == OrderType.LIMIT and status in (OrderStatus.NEW, OrderStatus.PARTIALLY_FILLED):
            self._resting[order_id] = _Resting(order, quantity - sum(t.quantity for t in trades))
        return order_id, trades

    def cancel(self, order_id: int) -> bool:
        cancelled = self._engine.cancel_order(order_id)
        if cancelled:
            del self._resting[order_id]
        return cancelled

    def reconcile(self, strategy_position: int) -> None:
        implied = 0
        for event in self.events:
            sign = 1 if event.trade.aggressor_side == Side.BUY else -1
            if event.aggressor_trader_id == STRATEGY_TRADER:
                implied += sign * event.trade.quantity
            if event.resting_trader_id == STRATEGY_TRADER:
                implied -= sign * event.trade.quantity
        if not implied == self.portfolio.position == strategy_position:
            raise AssertionError(f"Position mismatch: engine fills={implied}, portfolio={self.portfolio.position}, strategy={strategy_position}")


@dataclass(frozen=True)
class EquityPoint:
    timestamp: int
    close: float
    cash: float
    position: int
    equity: float


@dataclass(frozen=True)
class BarContext:
    history: tuple[Bar, ...]
    position: int
    cash: float
    _submit: object = field(repr=False)

    def market_order(self, side: Side, quantity: int) -> tuple[Trade, ...]:
        return self._submit(side, quantity)


class Strategy(Protocol):
    position: int

    def on_bar(self, bar: Bar, context: BarContext) -> None: ...
    def on_fill(self, fill: Fill) -> None: ...


@dataclass(frozen=True)
class ReplayResult:
    config: ReplayConfig
    portfolio: Portfolio
    events: tuple[TradeEvent, ...]
    equity_curve: tuple[EquityPoint, ...]
    self_trade_skips: int
    fok_rejections: int

    @property
    def ending_pnl(self) -> float:
        return self.equity_curve[-1].equity - self.config.initial_cash


class Replay:
    def __init__(self, config: ReplayConfig = ReplayConfig()) -> None:
        self.config = config

    def run(self, bars: tuple[Bar, ...] | list[Bar], strategy: Strategy) -> ReplayResult:
        if not bars:
            raise ValueError("Replay needs validated bars")
        if strategy.position != 0:
            raise ValueError("Replay requires a fresh, flat strategy")
        if any(a.end_timestamp > b.timestamp for a, b in zip(bars, bars[1:])):
            raise ValueError("Replay requires strictly ordered, non-overlapping bars; validate first")
        broker = Broker(self.config)
        broker.on_fill = strategy.on_fill
        history: list[Bar] = []
        equity: list[EquityPoint] = []
        quotes: list[int] = []
        for bar in bars:
            # No strategy callback occurs during this fabricated intrabar path.
            path = (bar.open, bar.low, bar.high, bar.close) if bar.close >= bar.open else (
                    bar.open, bar.high, bar.low, bar.close)
            pulse = min(MAX_QTY // 2, int(bar.volume * self.config.volume_fraction / (4 * self.config.unit_size)))
            prior = bar.open
            for index, pivot in enumerate(path):
                timestamp = bar.timestamp + (bar.end_timestamp - bar.timestamp - 1) * index // 3
                for order_id in quotes:
                    broker.cancel(order_id)
                quotes.clear()
                if pulse > 0:
                    half = max(self.config.tick_size, pivot * self.config.spread_bps / 20_000)
                    bid = max(self.config.tick_size, math.floor((pivot - half) / self.config.tick_size) * self.config.tick_size)
                    ask = max(bid + self.config.tick_size, math.ceil((pivot + half) / self.config.tick_size) * self.config.tick_size)
                    for side, price in ((Side.BUY, bid), (Side.SELL, ask)):
                        order_id, _ = broker.submit(trader_id=MAKER_TRADER, side=side, quantity=pulse * 2,
                                                   price=price, type=OrderType.LIMIT, timestamp=timestamp)
                        quotes.append(order_id)
                    broker.submit(trader_id=TAPE_TRADER, side=Side.BUY if pivot >= prior else Side.SELL,
                                  quantity=pulse, timestamp=timestamp)
                prior = pivot
            history.append(bar)
            callback_time = bar.end_timestamp

            def submit(side: Side, quantity: int, *, _time=callback_time):
                # Capture this callback's time so stale saved contexts cannot backdate orders.
                return broker.submit(trader_id=STRATEGY_TRADER, side=side, quantity=quantity,
                                     timestamp=_time)[1]

            context = BarContext(tuple(history), broker.portfolio.position, broker.portfolio.cash, submit)
            strategy.on_bar(bar, context)
            broker.reconcile(strategy.position)
            portfolio = broker.portfolio
            equity.append(EquityPoint(callback_time, bar.close, portfolio.cash, portfolio.position,
                                      portfolio.equity(bar.close)))
        return ReplayResult(self.config, broker.portfolio, tuple(broker.events), tuple(equity),
                            broker.self_trade_skips, broker.fok_rejections)
