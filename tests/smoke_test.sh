#!/usr/bin/env bash
set -euo pipefail

test_dir="$(mktemp -d)"
trap 'rm -rf "$test_dir"' EXIT

./build/wikisearch build --input tests/fixtures/tiny.jsonl --index "$test_dir/index" --limit 4 --batch-size 2

and_output="$(./build/wikisearch search --index "$test_dir/index" --query 'музей санкт' --top 10)"
grep -q 'Эрмитаж' <<<"$and_output"
grep -q 'https://ru.wikipedia.org/wiki/%D0%AD%D1%80%D0%BC%D0%B8%D1%82%D0%B0%D0%B6' <<<"$and_output"

or_output="$(./build/wikisearch search --index "$test_dir/index" --query 'москва OR эрмитаж' --top 10)"
grep -q 'Москва' <<<"$or_output"
grep -q 'Эрмитаж' <<<"$or_output"

unknown_output="$(./build/wikisearch search --index "$test_dir/index" --query 'несуществующийтерм' --top 10)"
grep -q 'found 0 result' <<<"$unknown_output"

ordered_and="$(./build/wikisearch search --index "$test_dir/index" --query 'россии город' --top 2)"
[[ "$(grep -c '^1\. \[1#0\]' <<<"$ordered_and")" -eq 1 ]]
[[ "$(grep -c '^2\. \[2#0\]' <<<"$ordered_and")" -eq 1 ]]

limited_and="$(./build/wikisearch search --index "$test_dir/index" --query 'россии город' --top 1)"
grep -q '^found 1 result' <<<"$limited_and"
grep -q '^1\. \[1#0\]' <<<"$limited_and"

ordered_or="$(./build/wikisearch search --index "$test_dir/index" --query 'эрмитаж OR нева' --top 2)"
[[ "$(grep -c '^1\. \[3#0\]' <<<"$ordered_or")" -eq 1 ]]
[[ "$(grep -c '^2\. \[4#0\]' <<<"$ordered_or")" -eq 1 ]]
grep -q 'https://example.org/neva' <<<"$ordered_or"

boolean_and_not="$(./build/wikisearch search --index "$test_dir/index" \
  --query '(москва OR эрмитаж) AND NOT музей' --top 10)"
grep -q 'Москва' <<<"$boolean_and_not"
! grep -q 'Эрмитаж' <<<"$boolean_and_not"

boolean_parentheses="$(./build/wikisearch search --index "$test_dir/index" \
  --query 'санкт AND (музей OR река)' --top 10)"
grep -q 'Эрмитаж' <<<"$boolean_parentheses"
grep -q 'Нева' <<<"$boolean_parentheses"

boolean_not="$(./build/wikisearch search --index "$test_dir/index" --query 'NOT москва' --top 10)"
grep -q '^found 3 result' <<<"$boolean_not"
! grep -q 'Москва' <<<"$boolean_not"

boolean_precedence="$(./build/wikisearch search --index "$test_dir/index" \
  --query 'москва OR эрмитаж AND музей' --top 10)"
grep -q '^found 2 result' <<<"$boolean_precedence"
grep -q 'Москва' <<<"$boolean_precedence"
grep -q 'Эрмитаж' <<<"$boolean_precedence"

boolean_implicit_and="$(./build/wikisearch search --index "$test_dir/index" \
  --query 'музей санкт' --top 10)"
grep -q '^found 1 result' <<<"$boolean_implicit_and"
grep -q 'Эрмитаж' <<<"$boolean_implicit_and"

boolean_double_not="$(./build/wikisearch search --index "$test_dir/index" \
  --query 'NOT NOT москва' --top 10)"
grep -q '^found 1 result' <<<"$boolean_double_not"
grep -q 'Москва' <<<"$boolean_double_not"

if ./build/wikisearch search --index "$test_dir/index" \
    --query 'москва AND (' --top 10 >/dev/null 2>&1; then
  echo "malformed boolean query unexpectedly succeeded" >&2
  exit 1
fi

cat >"$test_dir/queries.txt" <<'EOF'
# One Boolean expression per line.
(москва OR эрмитаж) AND NOT музей
санкт AND (музей OR река)
EOF

./build/wikisearch batch --index "$test_dir/index" \
  --input "$test_dir/queries.txt" --output "$test_dir/results.jsonl" --top 10
grep -q '"type":"query","line":2' "$test_dir/results.jsonl"
grep -q '"type":"query","line":3' "$test_dir/results.jsonl"
grep -q '"external_id":"2#0"' "$test_dir/results.jsonl"
grep -q '"external_id":"3#0"' "$test_dir/results.jsonl"
grep -q '"external_id":"4#0"' "$test_dir/results.jsonl"
grep -q '"type":"summary","queries":2,"succeeded":2,"failed":0' "$test_dir/results.jsonl"

echo "smoke test passed"

