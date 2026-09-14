"""Phase 2 CLI: raw candles -> validation -> native executions -> trade log and PnL."""
from __future__ import annotations

import argparse
from dataclasses import asdict
import hashlib
import json
import logging
from pathlib import Path
import sys

from .data import date_ns, fetch_binance, iso_time, load_binance, validate_bars
from .replay import Replay, ReplayConfig
from .strategies import MeanReversion, MovingAverageCrossover


def trade_dict(trade):
    return {"trade_id": trade.trade_id, "price": trade.price, "quantity": trade.quantity,
            "timestamp": trade.timestamp, "aggressor_order_id": trade.aggressor_order_id,
            "resting_order_id": trade.resting_order_id, "aggressor_side": trade.aggressor_side.name}


def _write_jsonl(path, rows):
    with path.open("w", encoding="utf-8") as stream:
        for row in rows:
            stream.write(json.dumps(row, allow_nan=False) + "\n")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--strategy", choices=("ma_crossover", "mean_reversion"), default="ma_crossover")
    parser.add_argument("--symbol", default="BTCUSDT", help="Binance symbol; BTCUSDT is BTC quoted in USDT, not BTC-USD")
    parser.add_argument("--start", default="2023-01-01")
    parser.add_argument("--end", default="2024-01-01", help="Exclusive UTC date")
    parser.add_argument("--data", type=Path, help="Existing raw Binance klines JSON; no network when provided")
    parser.add_argument("--output", type=Path, default=Path("runs/phase2"))
    parser.add_argument("--short-window", type=int, default=10)
    parser.add_argument("--long-window", type=int, default=30)
    parser.add_argument("--window", type=int, default=20)
    parser.add_argument("--entry-z", type=float, default=1.5)
    parser.add_argument("--quantity", type=int, default=100, help="Integer engine lots")
    parser.add_argument("--initial-cash", type=float, default=10_000)
    parser.add_argument("--fee-bps", type=float, default=5)
    parser.add_argument("--spread-bps", type=float, default=2)
    parser.add_argument("--unit-size", type=float, default=0.001, help="Base-asset units per engine lot")
    args = parser.parse_args(argv)
    logging.basicConfig(level=logging.WARNING, format="%(levelname)s %(message)s")
    try:
        start, end = date_ns(args.start), date_ns(args.end)
        if start >= end:
            raise ValueError("start must precede end")
        config = ReplayConfig(initial_cash=args.initial_cash, fee_bps=args.fee_bps,
                              spread_bps=args.spread_bps, unit_size=args.unit_size)
        if args.strategy == "ma_crossover":
            params = dict(short_window=args.short_window, long_window=args.long_window, quantity=args.quantity)
            strategy = MovingAverageCrossover(**params)
        else:
            params = dict(window=args.window, entry_z=args.entry_z, quantity=args.quantity)
            strategy = MeanReversion(**params)
        path = args.data or fetch_binance(args.symbol, args.start, args.end,
                    Path("data/cache") / f"{args.symbol}-{args.start}-{args.end}-1d.json")
        metadata_path = path.with_suffix(".metadata.json")
        if metadata_path.exists():
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            if metadata.get("symbol") != args.symbol:
                raise ValueError("Cached data symbol does not match --symbol")
            if metadata.get("sha256") != hashlib.sha256(path.read_bytes()).hexdigest():
                raise ValueError("Cached data SHA256 does not match its provenance metadata")
        validation = validate_bars(load_binance(path), start_ns=start, end_ns=end)
        result = Replay(config).run(validation.bars, strategy)
        args.output.mkdir(parents=True, exist_ok=True)
        _write_jsonl(args.output / "trades.jsonl", (
            {**trade_dict(f.trade), "simulation_timestamp": f.simulation_timestamp,
             "portfolio_side": f.side.name, "expected_mid": f.expected_mid,
             "slippage": f.slippage, "adverse_slippage": f.adverse_slippage, "fee": f.fee}
            for f in result.portfolio.fills))
        _write_jsonl(args.output / "events.jsonl", (
            {**trade_dict(e.trade), "simulation_timestamp": e.simulation_timestamp,
             "aggressor_trader_id": e.aggressor_trader_id, "resting_trader_id": e.resting_trader_id,
             "expected_mid": e.expected_mid} for e in result.events))
        _write_jsonl(args.output / "equity.jsonl", (asdict(e) for e in result.equity_curve))
        summary = {
            "phase": 2, "symbol": args.symbol, "strategy": args.strategy,
            "strategy_parameters": params, "config": asdict(config),
            "start_inclusive": args.start, "end_exclusive": args.end,
            "data_file": str(path), "data_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "bars": len(validation.bars), "validation_issues": [asdict(i) for i in validation.issues],
            "skipped_rows": validation.skipped_rows,
            "engine_trade_count": len(result.events), "strategy_fill_count": len(result.portfolio.fills),
            "ending_cash": result.portfolio.cash, "ending_position_lots": result.portfolio.position,
            "ending_equity": result.equity_curve[-1].equity, "ending_pnl": result.ending_pnl,
            "fees": result.portfolio.fees, "self_trade_skips": result.self_trade_skips,
            "fok_rejections": result.fok_rejections, "position_reconciled": True,
            "marking": "Final historical close; open inventory is not forcibly liquidated",
            "timestamps": "timestamp is unchanged C++ wall-clock time; simulation_timestamp is modeled historical arrival time",
        }
        (args.output / "summary.json").write_text(json.dumps(summary, indent=2, allow_nan=False), encoding="utf-8")
        print("historical_time                       trade_id side quantity price        fee      adverse_slippage")
        for fill in result.portfolio.fills:
            adverse = "n/a" if fill.adverse_slippage is None else f"{fill.adverse_slippage:.5f}"
            print(f"{iso_time(fill.simulation_timestamp)} {fill.trade.trade_id:8d} {fill.side.name:4s} "
                  f"{fill.trade.quantity:8d} {fill.trade.price:11.4f} {fill.fee:8.4f} {adverse}")
        print(f"{args.strategy}: {len(validation.bars)} bars, {len(result.portfolio.fills)} actual strategy fills; "
              f"position={result.portfolio.position} lots; fees={result.portfolio.fees:.4f}; "
              f"ending equity={result.equity_curve[-1].equity:.4f}; PnL={result.ending_pnl:.4f}")
        print(f"Logs: {args.output.resolve()}")
        return 0
    except (ValueError, OSError, OverflowError) as exc:
        print(f"Replay failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
