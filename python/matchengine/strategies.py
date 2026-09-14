"""Long/flat strategies; position changes exclusively in the real-fill callback."""
from __future__ import annotations

from collections import deque
import math
from statistics import fmean, pstdev

from . import Side
from .replay import BarContext, Fill, MAX_QTY


class _LongFlat:
    def __init__(self, quantity: int) -> None:
        if not isinstance(quantity, int) or not 1 <= quantity <= MAX_QTY:
            raise ValueError("quantity must be an integer in the engine's accepted range")
        self.quantity = quantity
        self.position = 0

    def on_fill(self, fill: Fill) -> None:
        self.position += fill.trade.quantity if fill.side == Side.BUY else -fill.trade.quantity

    def target(self, desired: int, context: BarContext) -> None:
        delta = desired - self.position
        if delta:
            context.market_order(Side.BUY if delta > 0 else Side.SELL, abs(delta))


class MovingAverageCrossover(_LongFlat):
    def __init__(self, short_window: int = 10, long_window: int = 30, quantity: int = 100) -> None:
        super().__init__(quantity)
        if not isinstance(short_window, int) or not isinstance(long_window, int) or not 1 <= short_window < long_window:
            raise ValueError("Require integer windows with 1 <= short_window < long_window")
        self.short_window, self.long_window = short_window, long_window
        self._prices: deque[float] = deque(maxlen=long_window)
        self._previous_difference: float | None = None

    def on_bar(self, bar, context: BarContext) -> None:
        self._prices.append(bar.close)
        if len(self._prices) < self.long_window:
            return
        prices = list(self._prices)
        difference = fmean(prices[-self.short_window:]) - fmean(prices)
        previous = self._previous_difference
        self._previous_difference = difference
        if previous is not None:
            if previous <= 0 < difference:
                self.target(self.quantity, context)
            elif previous >= 0 > difference:
                self.target(0, context)


class MeanReversion(_LongFlat):
    def __init__(self, window: int = 20, entry_z: float = 1.5, quantity: int = 100) -> None:
        super().__init__(quantity)
        if not isinstance(window, int) or window < 2 or not math.isfinite(entry_z) or entry_z <= 0:
            raise ValueError("Require window >= 2 and finite entry_z > 0")
        self.window, self.entry_z = window, entry_z
        self._prices: deque[float] = deque(maxlen=window)

    def on_bar(self, bar, context: BarContext) -> None:
        self._prices.append(bar.close)
        if len(self._prices) < self.window:
            return
        mean, deviation = fmean(self._prices), pstdev(self._prices)
        if deviation == 0:
            return
        z = (bar.close - mean) / deviation
        if z <= -self.entry_z:
            self.target(self.quantity, context)
        elif z >= 0:
            self.target(0, context)
