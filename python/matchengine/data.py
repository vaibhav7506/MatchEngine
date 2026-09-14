"""Public Binance daily candles, explicit validation, and reproducible raw caches."""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import logging
import math
from pathlib import Path
from urllib.parse import urlencode
from urllib.request import Request, urlopen

DAY_NS = 86_400_000_000_000
LOGGER = logging.getLogger(__name__)
ENDPOINT = "https://data-api.binance.vision/api/v3/klines"


@dataclass(frozen=True)
class Bar:
    timestamp: int  # inclusive open, ns UTC
    end_timestamp: int  # exclusive end, ns UTC
    open: float
    high: float
    low: float
    close: float
    volume: float  # base asset units


@dataclass(frozen=True)
class ValidationIssue:
    code: str
    timestamp: int
    message: str


@dataclass(frozen=True)
class ValidationResult:
    bars: tuple[Bar, ...]
    issues: tuple[ValidationIssue, ...]
    skipped_rows: int


def date_ns(value: str) -> int:
    """Parse a UTC date, deliberately avoiding floating-point nanosecond conversion."""
    dt = datetime.strptime(value, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    return int(dt.timestamp()) * 1_000_000_000


def iso_time(timestamp: int) -> str:
    seconds, ns = divmod(timestamp, 1_000_000_000)
    return datetime.fromtimestamp(seconds, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S") + f".{ns:09d}Z"


def load_binance(path: str | Path) -> list[Bar]:
    rows = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(rows, list):
        raise ValueError("Expected a Binance kline JSON array")
    result = []
    for index, row in enumerate(rows):
        try:
            result.append(Bar(int(row[0]) * 1_000_000, (int(row[6]) + 1) * 1_000_000,
                              *(float(row[i]) for i in range(1, 6))))
        except (ValueError, TypeError, IndexError) as exc:
            raise ValueError(f"Malformed Binance row {index}; no rows were silently dropped") from exc
    return result


def fetch_binance(symbol: str, start: str, end: str, destination: str | Path) -> Path:
    """Fetch daily spot candles, start inclusive / end exclusive, with pagination."""
    if not symbol.isalnum() or symbol != symbol.upper():
        raise ValueError("Use an uppercase Binance symbol such as BTCUSDT")
    start_ms, end_ms = date_ns(start) // 1_000_000, date_ns(end) // 1_000_000
    if start_ms >= end_ms:
        raise ValueError("start must precede end")
    cursor = start_ms
    rows, urls = [], []
    while cursor < end_ms:
        url = ENDPOINT + "?" + urlencode(dict(symbol=symbol, interval="1d", startTime=cursor,
                                               endTime=end_ms - 1, limit=1000))
        request = Request(url, headers={"User-Agent": "MatchEngine/0.2 (historical research)"})
        with urlopen(request, timeout=30) as response:
            batch = json.load(response)
        if not isinstance(batch, list):
            raise ValueError(f"Unexpected Binance response: {batch!r}")
        urls.append(url)
        if not batch:
            break
        rows.extend(batch)
        next_cursor = int(batch[-1][0]) + DAY_NS // 1_000_000
        if next_cursor <= cursor:
            raise ValueError("Binance pagination did not advance")
        cursor = next_cursor
    if not rows:
        raise ValueError("No historical candles returned")
    path = Path(destination)
    path.parent.mkdir(parents=True, exist_ok=True)
    raw = json.dumps(rows, separators=(",", ":")).encode()
    path.write_bytes(raw)
    path.with_suffix(".metadata.json").write_text(json.dumps({
        "provider": "Binance public spot klines", "symbol": symbol, "interval": "1d",
        "start_inclusive": start, "end_exclusive": end,
        "retrieved_at": datetime.now(timezone.utc).isoformat(), "urls": urls,
        "sha256": hashlib.sha256(raw).hexdigest(), "row_count": len(rows),
    }, indent=2), encoding="utf-8")
    return path


def validate_bars(bars: list[Bar] | tuple[Bar, ...], *, interval_ns: int = DAY_NS,
                  gap_threshold: float = 0.20, known_events: frozenset[int] = frozenset(),
                  start_ns: int | None = None, end_ns: int | None = None) -> ValidationResult:
    if interval_ns <= 0 or not math.isfinite(gap_threshold) or gap_threshold <= 0:
        raise ValueError("Interval and gap threshold must be positive")
    issues: list[ValidationIssue] = []

    def flag(code: str, timestamp: int, message: str) -> None:
        issues.append(ValidationIssue(code, timestamp, message))
        LOGGER.warning("%s at %s: %s", code, timestamp, message)

    accepted: dict[int, Bar] = {}
    skipped = 0
    previous = None
    for bar in bars:
        if previous is not None and bar.timestamp < previous:
            flag("non_monotonic", bar.timestamp, "Input is out of order; accepted rows will be sorted")
        previous = bar.timestamp
        prices = (bar.open, bar.high, bar.low, bar.close)
        if (not isinstance(bar.timestamp, int) or not isinstance(bar.end_timestamp, int)
                or not 0 < bar.timestamp < bar.end_timestamp < 2**64
                or bar.end_timestamp - bar.timestamp != interval_ns
                or not all(math.isfinite(p) and p > 0 for p in prices)
                or not math.isfinite(bar.volume) or bar.volume < 0
                or not bar.low <= min(bar.open, bar.close) <= max(bar.open, bar.close) <= bar.high):
            flag("invalid_bar", bar.timestamp, "Invalid OHLCV or timestamp interval; row skipped")
            skipped += 1
            continue
        if bar.timestamp in accepted:
            flag("duplicate", bar.timestamp, "Duplicate timestamp; first valid row retained")
            skipped += 1
            continue
        if ((start_ns is not None and bar.timestamp < start_ns)
                or (end_ns is not None and bar.end_timestamp > end_ns)):
            flag("outside_range", bar.timestamp, "Row outside requested date range; skipped")
            skipped += 1
            continue
        accepted[bar.timestamp] = bar
    ordered = tuple(accepted[t] for t in sorted(accepted))
    for prior, bar in zip(ordered, ordered[1:]):
        delta = bar.timestamp - prior.timestamp
        if delta != interval_ns:
            flag("missing_bars" if delta > interval_ns else "overlap", bar.timestamp,
                 f"Expected spacing {interval_ns} ns, observed {delta} ns; no candles imputed")
        gap = abs(bar.open / prior.close - 1)
        if gap > gap_threshold and bar.timestamp not in known_events:
            flag("outlier_gap", bar.timestamp, f"Open/previous-close gap {gap:.2%}; row retained")
    for bar in ordered:
        if abs(bar.close / bar.open - 1) > gap_threshold and bar.timestamp not in known_events:
            flag("outlier_return", bar.timestamp, "Large within-bar return; row retained")
    if not ordered:
        raise ValueError("No valid candles available for replay")
    if start_ns is not None and ordered[0].timestamp != start_ns:
        flag("missing_start", ordered[0].timestamp, "Requested leading candles are missing")
    if end_ns is not None and ordered[-1].end_timestamp != end_ns:
        flag("missing_end", ordered[-1].end_timestamp, "Requested trailing candles are missing")
    return ValidationResult(ordered, tuple(issues), skipped)
