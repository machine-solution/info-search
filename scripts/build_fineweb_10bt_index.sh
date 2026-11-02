#!/usr/bin/env bash
set -euo pipefail

readonly full_documents=14868862
readonly minimum_documents=10000000
readonly parquet_base="https://huggingface.co/datasets/HuggingFaceFW/fineweb/resolve/refs%2Fconvert%2Fparquet/sample-10BT/train"

limit="${1:-$full_documents}"
index_dir="${2:-index/fineweb-10bt-full}"
python_bin="${FINEWEB_PYTHON:-.venv-fineweb/bin/python}"

if [[ "$limit" -lt "$minimum_documents" ]]; then
  echo "the course requirement is at least $minimum_documents documents" >&2
  exit 2
fi
if [[ "$limit" -gt "$full_documents" ]]; then
  echo "FineWeb 10BT contains only $full_documents documents" >&2
  exit 2
fi
if [[ ! -x build/wikisearch ]]; then
  echo "build/wikisearch not found; run 'make' first" >&2
  exit 1
fi
if [[ ! -x "$python_bin" ]]; then
  echo "$python_bin not found; run scripts/setup_fineweb_adapter.sh" >&2
  exit 1
fi

available_kib="$(df -Pk . | awk 'NR == 2 {print $4}')"
if [[ "$available_kib" -lt $((55 * 1024 * 1024)) ]]; then
  echo "FineWeb build requires at least 55 GiB free; currently $((available_kib / 1024 / 1024)) GiB" >&2
  exit 1
fi

mkdir -p "$index_dir"
temp_dir="$(mktemp -d /tmp/wikisearch-fineweb.XXXXXX)"
temp_parquet="$temp_dir/shard.parquet"
cleanup() { rm -f "$temp_parquet"; rmdir "$temp_dir" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

stream_corpus() {
  for shard in $(seq -w 0 14); do
    local name="00${shard}.parquet"
    name="${name: -12}"
    local url="$parquet_base/$name"
    echo "downloading FineWeb shard $name" >&2
    curl --fail --location --retry 5 --continue-at - --output "$temp_parquet" "$url"
    "$python_bin" scripts/fineweb_parquet_to_jsonl.py "$temp_parquet"
    rm -f "$temp_parquet"
  done
}

set +e
stream_corpus | /usr/bin/time -v ./build/wikisearch build \
  --input - \
  --index "$index_dir" \
  --limit "$limit" \
  --batch-size 50000 \
  2> >(tee "$index_dir/build.log" >&2)
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
dataset=HuggingFaceFW/FineWeb sample-10BT
documents=$limit
full_configuration_documents=$full_documents
source=Common Crawl web pages filtered and deduplicated by FineWeb
language=English
license=ODC-By-1.0 dataset; source-page rights remain with their owners
selection=official sample-10BT order up to the requested document limit
raw_parquet_retained=false
source_urls_stored=true
EOF

echo "index written to $index_dir" >&2
du -sh "$index_dir" >&2
