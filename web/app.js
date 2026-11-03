const form = document.querySelector("#search-form");
const queryInput = document.querySelector("#query-input");
const limitInput = document.querySelector("#limit-input");
const searchButton = form.querySelector("button[type='submit']");
const resultsRoot = document.querySelector("#results");
const treeRoot = document.querySelector("#query-tree");
const errorBox = document.querySelector("#error-box");
const stateRoot = document.querySelector("#engine-state");
const stateText = document.querySelector("#engine-state-text");
const resultSummary = document.querySelector("#result-summary");

let activeRequest = null;

const integerFormatter = new Intl.NumberFormat("ru-RU");
const decimalFormatter = new Intl.NumberFormat("ru-RU", {
  minimumFractionDigits: 3,
  maximumFractionDigits: 3,
});

function setEngineState(kind, text) {
  stateRoot.classList.remove("ready", "failed");
  if (kind) stateRoot.classList.add(kind);
  stateText.textContent = text;
}

function showError(message) {
  errorBox.textContent = message;
  errorBox.hidden = false;
}

function clearError() {
  errorBox.hidden = true;
  errorBox.textContent = "";
}

function setLoading(loading) {
  searchButton.disabled = loading;
  searchButton.textContent = loading ? "Ищем…" : "Найти";
  if (!loading) return;
  resultsRoot.replaceChildren(...Array.from({ length: 3 }, () => {
    const row = document.createElement("div");
    row.className = "loading-row";
    return row;
  }));
  resultSummary.textContent = "Запрос исполняется…";
  showTreePlaceholder("Строится дерево запроса…");
}

function showTreePlaceholder(text) {
  const placeholder = document.createElement("div");
  placeholder.className = "tree-placeholder";
  placeholder.textContent = text;
  treeRoot.replaceChildren(placeholder);
}

function safeHttpUrl(value) {
  if (!value) return null;
  try {
    const url = new URL(value);
    return url.protocol === "http:" || url.protocol === "https:" ? url.href : null;
  } catch {
    return null;
  }
}

function renderResults(results) {
  resultsRoot.replaceChildren();
  if (results.length === 0) {
    const empty = document.createElement("div");
    empty.className = "empty-state";
    empty.textContent = "По этому выражению ничего не найдено";
    resultsRoot.append(empty);
    return;
  }

  const fragment = document.createDocumentFragment();
  for (const result of results) {
    const article = document.createElement("article");
    article.className = "result-card";

    const rank = document.createElement("div");
    rank.className = "result-rank";
    rank.textContent = String(result.rank).padStart(2, "0");

    const body = document.createElement("div");
    const title = document.createElement("a");
    title.className = "result-title";
    title.textContent = result.title || result.external_id || `Документ ${result.doc_id}`;
    const href = safeHttpUrl(result.url);
    if (href) {
      title.href = href;
      title.target = "_blank";
      title.rel = "noopener noreferrer";
    }
    body.append(title);

    if (href) {
      const url = document.createElement("span");
      url.className = "result-url";
      url.textContent = href;
      url.title = href;
      body.append(url);
    }

    const snippet = document.createElement("p");
    snippet.className = "result-snippet";
    snippet.textContent = result.snippet || "Текст документа отсутствует";
    body.append(snippet);

    const meta = document.createElement("div");
    meta.className = "result-meta";
    const internalId = document.createElement("span");
    internalId.textContent = `doc_id: ${result.doc_id}`;
    const externalId = document.createElement("span");
    externalId.textContent = `external_id: ${result.external_id || "—"}`;
    meta.append(internalId, externalId);
    body.append(meta);

    article.append(rank, body);
    fragment.append(article);
  }
  resultsRoot.append(fragment);
}

function buildTreeNode(node) {
  const item = document.createElement("li");
  const label = document.createElement("span");
  label.className = node.type === "TERM" ? "tree-node term" : "tree-node";
  label.textContent = node.type === "TERM" ? node.term : node.type;
  item.append(label);

  if (node.children?.length) {
    const children = document.createElement("ul");
    for (const child of node.children) children.append(buildTreeNode(child));
    item.append(children);
  }
  return item;
}

function renderTree(tree) {
  const root = document.createElement("ul");
  root.className = "tree";
  root.append(buildTreeNode(tree));
  treeRoot.replaceChildren(root);
}

async function loadMetadata() {
  try {
    const response = await fetch("/api/meta", { cache: "no-store" });
    const data = await response.json();
    if (!response.ok || !data.ok) throw new Error(data.error || "Индекс недоступен");
    setEngineState("ready", "Индекс загружен · сервер готов");
    return true;
  } catch (error) {
    setEngineState("failed", "Нет соединения с сервером");
    showError(`Не удалось получить сведения об индексе: ${error.message}`);
    return false;
  }
}

async function runSearch() {
  const query = queryInput.value.trim();
  const limit = Number.parseInt(limitInput.value, 10);
  if (!query) {
    showError("Введите поисковый запрос.");
    queryInput.focus();
    return;
  }
  if (!Number.isInteger(limit) || limit < 1 || limit > 100) {
    showError("Лимит должен быть целым числом от 1 до 100.");
    limitInput.focus();
    return;
  }

  activeRequest?.abort();
  activeRequest = new AbortController();
  clearError();
  setLoading(true);

  const parameters = new URLSearchParams({ query, limit: String(limit) });
  try {
    const response = await fetch(`/api/search?${parameters}`, {
      cache: "no-store",
      signal: activeRequest.signal,
    });
    const data = await response.json();
    if (!response.ok || !data.ok) throw new Error(data.error || "Поиск завершился с ошибкой");

    renderResults(data.results);
    renderTree(data.query_tree);
    resultSummary.textContent = `${integerFormatter.format(data.returned)} результатов · ` +
      `retrieval ${decimalFormatter.format(data.search_ms)} мс · ` +
      `всего ${decimalFormatter.format(data.total_ms)} мс`;
  } catch (error) {
    if (error.name === "AbortError") return;
    resultsRoot.replaceChildren();
    showTreePlaceholder("Дерево не построено");
    showError(error.message);
    resultSummary.textContent = "Запрос не выполнен";
  } finally {
    if (!activeRequest?.signal.aborted) setLoading(false);
  }
}

form.addEventListener("submit", (event) => {
  event.preventDefault();
  runSearch();
});

loadMetadata();
