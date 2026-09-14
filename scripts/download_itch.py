"""Download official Nasdaq BinaryFILE gzip samples and verify before decompressing.

No protocol parsing occurs here: the C++ itch_stats executable performs that step.
Only Python's standard library is required for downloading/gzip transport.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
import gzip
import hashlib
import json
from pathlib import Path
import time
from urllib.error import HTTPError
from urllib.request import Request, urlopen

SAMPLES = {
    "session": ("https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz", 3524013057),
    "noii": ("https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/NOII/S050922-v50-NOII.txt.gz", 96588423),
}


def digest(path, algorithm):
    result = hashlib.new(algorithm)
    with path.open("rb") as stream:
        while chunk := stream.read(4 * 1024 * 1024):
            result.update(chunk)
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sample", choices=SAMPLES, default="session")
    parser.add_argument("--directory", type=Path, default=Path("data/itch"))
    parser.add_argument("--workers", type=int, default=12)
    parser.add_argument("--decompress", action="store_true")
    parser.add_argument("--allow-unavailable-md5", action="store_true",
                        help="On an HTTP 404 for the listed checksum only, record it as unverified; gzip CRC remains mandatory")
    args = parser.parse_args()
    if not 1 <= args.workers <= 16:
        parser.error("workers must be 1..16")
    url, expected_size = SAMPLES[args.sample]
    args.directory.mkdir(parents=True, exist_ok=True)
    destination = args.directory / url.rsplit("/", 1)[1]
    started = time.monotonic()
    if not destination.exists():
        partial = destination.with_suffix(destination.suffix + ".partial")
        with partial.open("wb") as out:
            out.truncate(expected_size)
        chunk_size = 16 * 1024 * 1024

        def fetch(start):
            end = min(start + chunk_size, expected_size) - 1
            for attempt in range(3):
                try:
                    request = Request(url, headers={"Range": f"bytes={start}-{end}"})
                    with urlopen(request, timeout=180) as response, partial.open("r+b") as out:
                        if response.status != 206 or response.headers.get("Content-Range") != f"bytes {start}-{end}/{expected_size}":
                            raise ValueError("Server did not honor the exact byte range")
                        out.seek(start)
                        remaining = end - start + 1
                        while remaining:
                            data = response.read(min(1024 * 1024, remaining))
                            if not data:
                                raise ValueError("Truncated HTTP range")
                            out.write(data)
                            remaining -= len(data)
                        if response.read(1):
                            raise ValueError("HTTP range exceeded expected length")
                    return end - start + 1
                except (OSError, ValueError):
                    if attempt == 2:
                        raise
            raise AssertionError("unreachable")

        completed, reported = 0, 0
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            futures = [pool.submit(fetch, start) for start in range(0, expected_size, chunk_size)]
            for future in as_completed(futures):
                completed += future.result()
                if completed - reported >= 128 * 1024 * 1024 or completed == expected_size:
                    print(f"Downloaded {completed}/{expected_size} bytes in {time.monotonic() - started:.1f}s", flush=True)
                    reported = completed
        partial.replace(destination)
    if destination.stat().st_size != expected_size:
        raise ValueError("Download size differs from the official directory listing")
    metadata = {"url": url, "compressed_bytes": expected_size,
                "sha256": digest(destination, "sha256"),
                "verified_at": datetime.now(timezone.utc).isoformat()}
    if args.sample == "session":
        measured_md5 = digest(destination, "md5")
        try:
            with urlopen(url + ".md5sum", timeout=30) as response:
                official_md5 = response.read().decode("ascii").split()[0].lower()
            if measured_md5 != official_md5:
                raise ValueError("Official Nasdaq MD5 mismatch")
            metadata.update(md5=measured_md5, checksum_url=url + ".md5sum", official_md5_verified=True)
        except HTTPError as error:
            if error.code != 404 or not args.allow_unavailable_md5 or not args.decompress:
                raise
            metadata.update(md5=measured_md5, checksum_url=url + ".md5sum",
                            official_md5_verified=False, checksum_error="HTTP 404 from Nasdaq's listed checksum URL")
            print("Nasdaq MD5 URL returned 404; recording unverified publisher MD5 and requiring full gzip CRC validation.", flush=True)
    if args.decompress:
        raw = destination.with_suffix("")
        partial_raw = raw.with_suffix(raw.suffix + ".partial")
        raw_hash = hashlib.sha256()
        size = 0
        reported = 0
        with gzip.open(destination, "rb") as source, partial_raw.open("wb") as output:
            while block := source.read(4 * 1024 * 1024):
                output.write(block)
                raw_hash.update(block)
                size += len(block)
                if size - reported >= 1024 * 1024 * 1024:
                    print(f"Decompressed {size} bytes", flush=True)
                    reported = size
        # Reading to gzip EOF verifies its CRC and trailer, including truncation.
        partial_raw.replace(raw)
        metadata.update(raw_file=raw.name, raw_bytes=size, raw_sha256=raw_hash.hexdigest(), gzip_crc_verified=True)
    metadata_path = args.directory / (destination.name + ".metadata.json")
    metadata_path.write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    print(json.dumps(metadata, indent=2), flush=True)


if __name__ == "__main__":
    main()
