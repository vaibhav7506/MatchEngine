import hashlib
import io
import json
from urllib.parse import parse_qs, urlparse

import pytest

from matchengine import data
from matchengine.cli import main


def raw_row(open_ms, price=100):
    return [open_ms, str(price), str(price * 1.01), str(price * .99), str(price),
            "1000", open_ms + 86_400_000 - 1, "0", 1, "0", "0", "0"]


def test_public_downloader_paginates_and_records_provenance(monkeypatch, tmp_path):
    first = data.date_ns("2023-01-01") // 1_000_000
    seen = []

    def response(request, timeout):
        query = parse_qs(urlparse(request.full_url).query)
        seen.append(query)
        assert timeout == 30
        assert query["symbol"] == ["BTCUSDT"]
        assert query["interval"] == ["1d"]
        return io.StringIO(json.dumps([raw_row(int(query["startTime"][0]))]))

    monkeypatch.setattr(data, "urlopen", response)
    path = data.fetch_binance("BTCUSDT", "2023-01-01", "2023-01-03", tmp_path / "candles.json")
    assert [q["startTime"] for q in seen] == [[str(first)], [str(first + 86_400_000)]]
    assert all(q["endTime"] == [str(first + 2 * 86_400_000 - 1)] for q in seen)
    bars = data.load_binance(path)
    assert len(bars) == 2 and not data.validate_bars(bars).issues
    metadata = json.loads(path.with_suffix(".metadata.json").read_text())
    assert metadata["sha256"] == hashlib.sha256(path.read_bytes()).hexdigest()
    assert metadata["row_count"] == 2 and metadata["symbol"] == "BTCUSDT"


def test_downloader_rejects_stalled_pagination(monkeypatch, tmp_path):
    first = data.date_ns("2023-01-01") // 1_000_000
    monkeypatch.setattr(data, "urlopen", lambda *_a, **_k: io.StringIO(json.dumps([raw_row(first)])))
    with pytest.raises(ValueError, match="did not advance"):
        data.fetch_binance("BTCUSDT", "2023-01-01", "2023-01-03", tmp_path / "candles.json")


def test_cli_writes_real_fills_and_reconciled_equity(tmp_path, capsys):
    first = data.date_ns("2023-01-01") // 1_000_000
    path = tmp_path / "input.json"
    path.write_text(json.dumps([raw_row(first + i * 86_400_000, p) for i, p in enumerate([10, 10, 7, 10])]))
    output = tmp_path / "output"
    assert main(["--data", str(path), "--output", str(output), "--strategy", "mean_reversion",
                 "--start", "2023-01-01", "--end", "2023-01-05", "--window", "3", "--entry-z", "1", "--quantity", "5"]) == 0
    summary = json.loads((output / "summary.json").read_text())
    fills = [json.loads(line) for line in (output / "trades.jsonl").read_text().splitlines()]
    events = [json.loads(line) for line in (output / "events.jsonl").read_text().splitlines()]
    equity = [json.loads(line) for line in (output / "equity.jsonl").read_text().splitlines()]
    assert summary["position_reconciled"] is True
    assert summary["strategy_fill_count"] == len(fills) == 2
    assert summary["engine_trade_count"] == len(events) == 18
    assert fills[0]["portfolio_side"] == "BUY" and fills[1]["portfolio_side"] == "SELL"
    assert all(f["timestamp"] != f["simulation_timestamp"] for f in fills)
    assert all(f["trade_id"] in {e["trade_id"] for e in events} for f in fills)
    assert equity[-1]["position"] == 0
    assert summary["ending_pnl"] == pytest.approx(equity[-1]["equity"] - 10000)
    assert "actual strategy fills" in capsys.readouterr().out


def test_cli_rejects_wrong_cached_symbol(tmp_path):
    path = tmp_path / "input.json"
    path.write_text("[]")
    path.with_suffix(".metadata.json").write_text(json.dumps({"symbol": "ETHUSDT"}))
    assert main(["--data", str(path), "--symbol", "BTCUSDT"]) == 1


def test_outlier_first_bar_is_flagged():
    first = data.date_ns("2023-01-01")
    bar = data.Bar(first, first + data.DAY_NS, 100, 150, 99, 150, 1000)
    assert [i.code for i in data.validate_bars([bar]).issues] == ["outlier_return"]
