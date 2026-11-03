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
const pagination = document.querySelector("#pagination");
const previousPage = document.querySelector("#previous-page");
const nextPage = document.querySelector("#next-page");
const pageNumber = document.querySelector("#page-number");

let activeRequest = null;
let cursor = null;
let pages = [];
let currentPage = -1;

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

function setLoading(loading, resetTree = false) {
  searchButton.disabled = loading;
  searchButton.textContent = loading ? "Ищем…" : "Найти";
  if (!loading) {
    const current = pages[currentPage];
    previousPage.disabled = currentPage <= 0;
    nextPage.disabled = !current ||
      (currentPage === pages.length - 1 && !current.has_more);
    return;
  }
  previousPage.disabled = true;
  nextPage.disabled = true;
  resultsRoot.replaceChildren(...Array.from({ length: 3 }, () => {
    const row = document.createElement("div");
    row.className = "loading-row";
    return row;
  }));
  resultSummary.textContent = "Запрос исполняется…";
  if (resetTree) showTreePlaceholder("Строится дерево запроса…");
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

function renderPage(index) {
  const data = pages[index];
  currentPage = index;
  renderResults(data.results);
  if (data.query_tree) renderTree(data.query_tree);

  const first = data.results.at(0)?.rank;
  const last = data.results.at(-1)?.rank;
  const range = first === undefined ? "0 результатов" :
    first === last ? `результат ${integerFormatter.format(first)}` :
      `результаты ${integerFormatter.format(first)}–${integerFormatter.format(last)}`;
  resultSummary.textContent = `Страница ${integerFormatter.format(data.page)} · ${range} · ` +
    `retrieval ${decimalFormatter.format(data.search_ms)} мс · ` +
    `всего ${decimalFormatter.format(data.total_ms)} мс`;

  pagination.hidden = false;
  pageNumber.textContent = `Страница ${integerFormatter.format(data.page)}`;
  previousPage.disabled = index === 0;
  nextPage.disabled = index === pages.length - 1 && !data.has_more;
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
  const pageSize = Number.parseInt(limitInput.value, 10);
  if (!query) {
    showError("Введите поисковый запрос.");
    queryInput.focus();
    return;
  }
  if (!Number.isInteger(pageSize) || pageSize < 1 || pageSize > 1000) {
    showError("Размер страницы должен быть целым числом от 1 до 1000.");
    limitInput.focus();
    return;
  }

  activeRequest?.abort();
  activeRequest = new AbortController();
  cursor = null;
  pages = [];
  currentPage = -1;
  pagination.hidden = true;
  clearError();
  setLoading(true, true);

  const parameters = new URLSearchParams({ query, page_size: String(pageSize) });
  try {
    const response = await fetch(`/api/search?${parameters}`, {
      cache: "no-store",
      signal: activeRequest.signal,
    });
    const data = await response.json();
    if (!response.ok || !data.ok) throw new Error(data.error || "Поиск завершился с ошибкой");

    cursor = data.cursor;
    pages.push(data);
    renderPage(0);
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

async function loadNextPage() {
  if (currentPage + 1 < pages.length) {
    renderPage(currentPage + 1);
    return;
  }
  const current = pages[currentPage];
  if (!current?.has_more || !cursor) return;

  activeRequest?.abort();
  activeRequest = new AbortController();
  clearError();
  setLoading(true);
  try {
    const parameters = new URLSearchParams({ cursor });
    const response = await fetch(`/api/search?${parameters}`, {
      cache: "no-store",
      signal: activeRequest.signal,
    });
    const data = await response.json();
    if (!response.ok || !data.ok) throw new Error(data.error || "Не удалось получить страницу");
    pages.push(data);
    renderPage(pages.length - 1);
  } catch (error) {
    if (error.name === "AbortError") return;
    showError(error.message === "search cursor expired"
      ? "Сессия поиска истекла. Запустите запрос ещё раз."
      : error.message);
    renderPage(currentPage);
  } finally {
    if (!activeRequest?.signal.aborted) setLoading(false);
  }
}

form.addEventListener("submit", (event) => {
  event.preventDefault();
  runSearch();
});

previousPage.addEventListener("click", () => {
  if (currentPage > 0) renderPage(currentPage - 1);
});

nextPage.addEventListener("click", loadNextPage);

loadMetadata();
