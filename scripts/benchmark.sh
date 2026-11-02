#!/usr/bin/env bash
set -euo pipefail

index_dir="${1:-index/learning-web-10m}"
queries="${2:-tests/benchmark_queries_learning_web.txt}"

if [[ ! -x build/wikisearch ]]; then
  echo "build/wikisearch not found; run 'make' first" >&2
  exit 1
fi
if [[ ! -f "$index_dir/metadata.txt" ]]; then
  echo "index not found: $index_dir" >&2
  exit 1
fi

result_file="$(mktemp)"
times_file="$(mktemp)"
trap 'rm -f "$result_file" "$times_file"' EXIT

{
  cat "$queries"
  printf '\n'
} | ./build/wikisearch search --index "$index_dir" --top 1 \
  >"$result_file"

awk '/query time:/ {print $(NF - 1)}' "$result_file" | sort -n >"$times_file"

awk '
  { values[NR] = $1; sum += $1 }
  END {
    if (NR == 0) { print "no timings found" > "/dev/stderr"; exit 1 }
    p50 = int((NR + 1) * 0.50); if (p50 < 1) p50 = 1
    p95 = int((NR + 1) * 0.95); if (p95 > NR) p95 = NR
    printf "queries=%d\n", NR
    printf "mean_ms=%.3f\n", sum / NR
    printf "p50_ms=%.3f\n", values[p50]
    printf "p95_ms=%.3f\n", values[p95]
    printf "max_ms=%.3f\n", values[NR]
  }
' "$times_file"

