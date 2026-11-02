# Булевые запросы и batch-режим

## Синтаксис

Поддерживаются `AND`, `OR`, `NOT` и круглые скобки. Операторы не зависят от
регистра. Приоритет от самого сильного к самому слабому:

1. `NOT`
2. `AND`
3. `OR`

Поэтому `alpha OR beta AND NOT gamma` означает
`alpha OR (beta AND (NOT gamma))`. Соседние операнды связываются неявным `AND`,
то есть `artificial intelligence` эквивалентно `artificial AND intelligence`.

Примеры:

```text
artificial AND intelligence
(machine OR deep) AND learning
climate AND change AND NOT fiction
NOT (fiction OR poetry)
```

Двойные кавычки делают текст литеральным. Например, `"and"` ищет слово `and`, а не
оператор. Это не phrase search: `"machine learning"` пока означает обычный AND двух
термов, потому что позиции слов появятся только во втором майлстоуне.

## Один запрос

```bash
./build/wikisearch search \
  --index index/learning-web-10m \
  --query '(machine OR deep) AND learning AND NOT fiction' \
  --top 10
```

Если `--query` не передать, запустится интерактивный режим. Все запросы проходят
через один булев парсер и дерево итераторов. Соседние термы означают неявный `AND`.

## Пакет запросов

Входной файл — UTF-8 текст, одна формула на строку. Пустые строки и строки,
начинающиеся с `#`, пропускаются.

```bash
./build/wikisearch batch \
  --index index/learning-web-10m \
  --input examples/boolean_queries.txt \
  --output results.jsonl \
  --top 10
```

Можно стримить оба конца:

```bash
printf '%s\n' 'search AND engine' 'search AND NOT engine' |
  ./build/wikisearch batch \
    --index index/learning-web-10m --input - --output - --top 5
```

На каждую формулу записывается JSON-строка с полями:

- `line`, `query`, `ok`;
- `returned`, `search_ms`, `total_ms`;
- `results`: rank, внутренний `doc_id`, внешний ID, title, URL и snippet.

`search_ms` измеряет только вычисление top-K ID. `total_ms` дополнительно включает чтение и
распаковку документов, но не запись выходной строки. Последняя JSON-строка имеет тип
`summary` и содержит число успешных/ошибочных запросов, mean, p50, p95, max и полное время
пакета.

Синтаксическая ошибка одной формулы записывается как `ok:false`, остальные строки всё равно
выполняются. После такого пакета команда возвращает код 3.

## Почему исполнение эффективно

- Полные postings lists не загружаются в RAM.
- Каждый узел дерева выдаёт отсортированный поток ID и не хранит весь промежуточный ответ.
- AND начинает с самого короткого postings list.
- OR использует min-heap и сразу дедуплицирует ID.
- `A AND NOT B` работает как разность двух потоков, а не как материализованный список
  из миллионов документов.
- Вычисление прекращается сразу после top-K.
