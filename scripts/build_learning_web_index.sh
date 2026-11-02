#!/usr/bin/env bash
set -euo pipefail

readonly edu_documents=9672101
readonly regular_documents=1000000
readonly total_documents=$((edu_documents + regular_documents))
readonly minimum_documents=10000000
readonly edu_base="https://huggingface.co/datasets/HuggingFaceFW/fineweb-edu/resolve/refs%2Fconvert%2Fparquet/sample-10BT/train"
readonly regular_url="https://huggingface.co/datasets/HuggingFaceFW/fineweb/resolve/refs%2Fconvert%2Fparquet/sample-10BT/train/0000.parquet"

index_dir="${1:-index/learning-web-10m}"
python_bin="${FINEWEB_PYTHON:-.venv-fineweb/bin/python}"
regular_shard="${2:-${FINEWEB_REGULAR_SHARD:-}}"

if [[ "$total_documents" -lt "$minimum_documents" ]]; then
  echo "internal corpus configuration is below the course minimum" >&2
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
if [[ -n "$regular_shard" && ! -f "$regular_shard" ]]; then
  echo "FINEWEB_REGULAR_SHARD does not exist: $regular_shard" >&2
  exit 1
fi

available_kib="$(df -Pk . | awk 'NR == 2 {print $4}')"
if [[ "$available_kib" -lt $((45 * 1024 * 1024)) ]]; then
  echo "build requires at least 45 GiB free; currently $((available_kib / 1024 / 1024)) GiB" >&2
  exit 1
fi

mkdir -p "$index_dir"
temp_dir="$(mktemp -d /tmp/wikisearch-learning-web.XXXXXX)"
temp_parquet="$temp_dir/shard.parquet"
cleanup() { rm -f "$temp_parquet"; rmdir "$temp_dir" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

download_and_convert() {
  local url="$1"
  local attempt
  rm -f "$temp_parquet"
  touch "$temp_parquet"
  echo "downloading $url (resumable)" >&2
  for attempt in $(seq 1 20); do
    if curl \
      --fail \
      --location \
      --continue-at - \
      --retry 10 \
      --retry-delay 3 \
      --retry-all-errors \
      --connect-timeout 30 \
      --speed-limit 1024 \
      --speed-time 120 \
      --output "$temp_parquet" \
      "$url" && \
      "$python_bin" -c \
        'import sys; import pyarrow.parquet as pq; assert pq.ParquetFile(sys.argv[1]).metadata.num_rows > 0' \
        "$temp_parquet"; then
      break
    fi
    if [[ "$attempt" -eq 20 ]]; then
      echo "failed to download a valid Parquet shard after $attempt attempts: $url" >&2
      return 1
    fi
    echo "download interrupted; resuming attempt $((attempt + 1))/20" >&2
  done
  "$python_bin" scripts/fineweb_parquet_to_jsonl.py "$temp_parquet"
  rm -f "$temp_parquet"
}

stream_corpus() {
  for shard in $(seq -w 0 13); do
    local name="00${shard}.parquet"
    name="${name: -12}"
    download_and_convert "$edu_base/$name" || return $?
  done

  if [[ -n "$regular_shard" ]]; then
    echo "using local FineWeb regular shard $regular_shard" >&2
    "$python_bin" scripts/fineweb_parquet_to_jsonl.py "$regular_shard" || return $?
  else
    download_and_convert "$regular_url" || return $?
  fi
}

set +e
stream_corpus | /usr/bin/time -v ./build/wikisearch build \
  --input - \
  --index "$index_dir" \
  --limit "$total_documents" \
  --batch-size 50000 \
  2> >(tee "$index_dir/build.log" >&2)
pipeline_status=("${PIPESTATUS[@]}")
set -e

if [[ "${pipeline_status[1]}" -ne 0 ]]; then
  exit "${pipeline_status[1]}"
fi

actual="$(sed -n 's/^documents=//p' "$index_dir/metadata.txt")"
if [[ "$actual" != "$total_documents" ]]; then
  echo "expected $total_documents documents, received $actual" >&2
  exit 1
fi
# The final regular-FineWeb shard contains more rows than requested. Once the
# C++ builder reaches the exact document limit it closes stdin; the producer
# then exits with SIGPIPE (128 + 13 = 141). The exact count above makes this a
# successful, intentional early stop rather than a truncated corpus.
if [[ "${pipeline_status[0]}" -ne 0 && "${pipeline_status[0]}" -ne 141 ]]; then
  echo "corpus stream failed with status ${pipeline_status[0]}" >&2
  exit "${pipeline_status[0]}"
fi

cat >"$index_dir/corpus.txt" <<EOF
dataset=Learning Web 10M
documents=$total_documents
fineweb_edu_documents=$edu_documents
fineweb_regular_documents=$regular_documents
source=FineWeb-Edu sample-10BT plus the first 1,000,000 FineWeb sample-10BT rows
language=English
license=ODC-By-1.0 datasets; source-page rights remain with their owners
raw_parquet_retained=false
source_urls_stored=true
EOF

echo "index written to $index_dir" >&2
du -sh "$index_dir" >&2
