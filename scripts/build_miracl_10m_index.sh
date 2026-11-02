#!/usr/bin/env bash
set -euo pipefail

limit="${1:-10000000}"
index_dir="${2:-index/miracl-wikipedia-10m}"
base_url="https://huggingface.co/datasets/miracl/miracl-corpus/resolve/main"

if [[ "$limit" -lt 10000000 ]]; then
  echo "the course requirement is at least 10,000,000 documents" >&2
  exit 2
fi
if [[ ! -x build/wikisearch ]]; then
  echo "build/wikisearch not found; run 'make' first" >&2
  exit 1
fi

mkdir -p "$(dirname "$index_dir")"

download_shard() {
  local language="$1"
  local shard="$2"
  local url="$base_url/miracl-corpus-v1.0-$language/docs-$shard.jsonl.gz"
  echo "streaming MIRACL-$language shard $shard" >&2
  curl --fail --location --retry 3 --silent --show-error "$url" |
    gzip --decompress --stdout |
    sed -E "s/\"docid\"[[:space:]]*:[[:space:]]*\"/\"docid\": \"$language:/"
}

stream_corpus() {
  # MIRACL-RU has 20 shards and 9,543,918 passages in total.
  for shard in $(seq 0 19); do
    if ! download_shard ru "$shard"; then return 0; fi
  done
  # Add only as much English Wikipedia as needed to cross 10 million unique docs.
  for shard in $(seq 0 2); do
    if ! download_shard en "$shard"; then return 0; fi
  done
}

set +e
stream_corpus | ./build/wikisearch build \
  --input - \
  --index "$index_dir" \
  --limit "$limit" \
  --batch-size 50000
pipeline_status=("${PIPESTATUS[@]}")
set -e

if [[ "${pipeline_status[1]}" -ne 0 ]]; then
  exit "${pipeline_status[1]}"
fi

actual="$(sed -n 's/^documents=//p' "$index_dir/metadata.txt")"
if [[ "$actual" != "$limit" ]]; then
  echo "expected $limit documents, received $actual; download was incomplete" >&2
  exit 1
fi

cat >"$index_dir/corpus.txt" <<EOF
dataset=MIRACL Wikipedia passages
documents=$limit
russian_documents=9543918
english_documents=$((limit - 9543918))
selection=all MIRACL-RU followed by MIRACL-EN passages up to the limit
raw_archives_retained=false
EOF

echo "index written to $index_dir" >&2
du -sh "$index_dir" >&2

