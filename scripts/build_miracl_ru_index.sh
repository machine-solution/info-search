#!/usr/bin/env bash
set -euo pipefail

limit="${1:-1000000}"
index_dir="${2:-index/miracl-ru-1m}"
shards="${MIRACL_SHARDS:-0 1 2}"
base_url="https://huggingface.co/datasets/miracl/miracl-corpus/resolve/main/miracl-corpus-v1.0-ru"

if [[ ! -x build/wikisearch ]]; then
  echo "build/wikisearch not found; run 'make' first" >&2
  exit 1
fi

mkdir -p "$(dirname "$index_dir")"

stream_corpus() {
  for shard in $shards; do
    echo "downloading MIRACL-RU shard $shard" >&2
    if ! curl --fail --location --retry 3 --silent --show-error \
      "$base_url/docs-$shard.jsonl.gz" | gzip --decompress --stdout; then
      # Usually this is the expected broken pipe after the indexer has reached
      # its document limit. The exact count is validated below.
      return 0
    fi
  done
}

set +e
stream_corpus | ./build/wikisearch build \
  --input - \
  --index "$index_dir" \
  --limit "$limit" \
  --batch-size 25000
pipeline_status=("${PIPESTATUS[@]}")
set -e

# The indexer deliberately closes stdin after reaching the limit. curl/gzip can
# therefore report a broken pipe even though the requested corpus was built.
if [[ "${pipeline_status[1]}" -ne 0 ]]; then
  exit "${pipeline_status[1]}"
fi

actual="$(sed -n 's/^documents=//p' "$index_dir/metadata.txt")"
if [[ "$actual" != "$limit" ]]; then
  echo "expected $limit documents, received $actual; download was incomplete" >&2
  exit 1
fi

echo "index written to $index_dir" >&2
du -sh "$index_dir" >&2

