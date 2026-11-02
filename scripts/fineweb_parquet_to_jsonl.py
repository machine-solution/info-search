#!/usr/bin/env python3
"""Stream a FineWeb Parquet shard as the generic JSONL accepted by Wikisearch."""

from __future__ import annotations

import argparse
import json
import re
import signal
import sys
from urllib.parse import urlsplit

import pyarrow.parquet as pq


_SPACE = re.compile(r"\s+")


def document_title(text: str, url: str, maximum: int = 180) -> str:
    for line in text.splitlines():
        title = _SPACE.sub(" ", line).strip()
        if title:
            if len(title) > maximum:
                return title[: maximum - 1].rstrip() + "…"
            return title
    parsed = urlsplit(url)
    fallback = (parsed.netloc + parsed.path).strip("/")
    return fallback[:maximum] or "Untitled web document"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("parquet", help="FineWeb parquet shard")
    parser.add_argument("--batch-size", type=int, default=2048)
    args = parser.parse_args()

    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    output = sys.stdout
    rows = 0
    source_tokens = 0
    parquet = pq.ParquetFile(args.parquet)
    columns = ["id", "text", "url", "token_count"]
    for batch in parquet.iter_batches(batch_size=args.batch_size, columns=columns):
        for row in batch.to_pylist():
            text = row["text"] or ""
            url = row["url"] or ""
            external_id = "fineweb:" + (row["id"] or str(rows))
            record = {
                "id": external_id,
                "title": document_title(text, url),
                "url": url if url.startswith(("http://", "https://")) else "",
                "text": text,
            }
            output.write(json.dumps(record, ensure_ascii=False, separators=(",", ":")))
            output.write("\n")
            rows += 1
            source_tokens += int(row["token_count"] or 0)

    print(
        f"converted {rows} FineWeb documents, source_token_count={source_tokens}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
