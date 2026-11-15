(() => {
  "use strict";

  const PAGE_SIZE = 24;
  const SCROLL_THRESHOLD = 600;
  const SCAN_POLL_INTERVAL = 2500;
  const SCAN_POLL_ATTEMPTS = 24;
  // 与服务端一致，只检查 RFC3339 的形状。
  const TIMESTAMP_PATTERN = /^\d{4}-\d{2}-\d{2}[Tt]\d{2}:\d{2}:\d{2}/;

  const ORPHAN_QUERY = `
    query OrphanImages($limit: Int!, $offset: Int!, $includeTotal: Boolean!) {
      orphanImages(limit: $limit, offset: $offset, includeTotal: $includeTotal) {
        items {
          id
          width
          height
          frameCount
          createdAt
          scanAt
        }
        hasMore
        total
      }
    }
  `;

  const ADD_MUTATION = `
    mutation AddFromImages($input: AddArtworkFromImagesInput!) {
      addArtworkFromImages(input: $input)
    }
  `;

  const grid = document.getElementById("orphan-grid");
  const status = document.getElementById("orphan-status");
  const notice = document.getElementById("orphan-notice");
  const empty = document.getElementById("orphan-empty");
  const scrollStatus = document.getElementById("orphan-scroll-status");
  const addButton = document.getElementById("orphan-add-button");

  const addDialog = document.getElementById("orphan-add-dialog");
  const addForm = document.getElementById("orphan-add-form");
  const addCount = document.getElementById("orphan-add-count");
  const addTitle = document.getElementById("orphan-add-title");
  const addTitleHint = document.getElementById("orphan-add-title-hint");
  const addDescription = document.getElementById("orphan-add-description");
  const addPublished = document.getElementById("orphan-add-published");
  const addPublishedField = document.getElementById(
    "orphan-add-published-field",
  );
  const addRescan = document.getElementById("orphan-add-rescan");
  const addError = document.getElementById("orphan-add-error");
  const addSubmit = document.getElementById("orphan-add-submit");
  const addCancel = document.getElementById("orphan-add-cancel");
  const addClose = document.getElementById("orphan-add-close");
  const addModeInputs = Array.from(
    document.querySelectorAll('input[name="orphan-add-mode"]'),
  );
  const addModeHint = document.getElementById("orphan-add-mode-hint");

  const tagInput = document.getElementById("orphan-add-tag-input");
  const tagSuggest = document.getElementById("orphan-add-tag-suggest");
  const tagStage = document.getElementById("orphan-add-tag-stage");

  // 列表与选择状态。
  const items = [];
  const cards = new Map();
  const selected = new Set();
  let nextOffset = 0;
  let hasMore = true;
  let loading = false;
  let total = null;
  let loadVersion = 0;

  // 标签联想与暂存区。
  const tags = [];
  const tagSuggestWidget = TagSearch.create({
    input: tagInput,
    list: tagSuggest,
    idPrefix: "orphan-tag",
    onEnter: () => addTagFromInput(),
  });

  async function graphql(query, variables, signal) {
    const response = await fetch("/graphql", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ query, variables }),
      signal,
    });

    let payload = null;
    try {
      payload = await response.json();
    } catch {
      payload = null;
    }

    if (!response.ok) {
      const message =
        payload && payload.errors && payload.errors.length
          ? payload.errors[0].message
          : `HTTP ${response.status}`;
      throw new Error(message);
    }
    if (Array.isArray(payload?.errors) && payload.errors.length > 0) {
      throw new Error(payload.errors[0].message || "请求失败");
    }
    return payload?.data || {};
  }

  function formatDate(value) {
    if (!value) {
      return "";
    }
    // 服务端返回 UTC 秒级 RFC3339，只做展示，不转换时区。
    return value.replace("T", " ").replace("Z", "").slice(0, 16);
  }

  function shortId(value) {
    return typeof value === "string" ? value.slice(0, 8) : "";
  }

  function delay(milliseconds) {
    return new Promise((resolve) => {
      window.setTimeout(resolve, milliseconds);
    });
  }

  function setNotice(text, isError) {
    notice.textContent = text;
    notice.hidden = !text;
    notice.classList.toggle("error", Boolean(isError));
  }

  function setScrollStatus(text, isError) {
    scrollStatus.textContent = text;
    scrollStatus.hidden = !text;
    scrollStatus.classList.toggle("error", Boolean(isError));
  }

  function updateStatus() {
    if (total === null && selected.size === 0) {
      status.hidden = true;
      return;
    }
    const parts = [];
    if (total !== null) {
      parts.push(`共 ${total} 张`);
    }
    parts.push(`已选 ${selected.size} 张`);
    status.textContent = parts.join(" · ");
    status.hidden = false;
  }

  function updateAddButton() {
    addButton.disabled = selected.size === 0;
    addButton.textContent =
      selected.size > 0 ? `添加 (${selected.size})` : "添加";
  }

  function refreshSelection() {
    for (const [id, card] of cards) {
      const on = selected.has(id);
      card.classList.toggle("selected", on);
      card.setAttribute("aria-pressed", on ? "true" : "false");
    }
    updateStatus();
    updateAddButton();
  }

  function selectOnly(id) {
    selected.clear();
    selected.add(id);
    refreshSelection();
  }

  function toggleSelection(id) {
    if (selected.has(id)) {
      selected.delete(id);
    } else {
      selected.add(id);
    }
    refreshSelection();
  }

  function clearSelection() {
    selected.clear();
    refreshSelection();
  }

  function selectedItems() {
    return items.filter((item) => selected.has(item.id));
  }

  function makePlaceholder(text) {
    const span = document.createElement("span");
    span.className = "orphan-placeholder-text";
    span.textContent = text;
    return span;
  }

  function createCard(item) {
    const card = document.createElement("button");
    card.type = "button";
    card.className = "card orphan-card";
    card.dataset.imageId = item.id;
    card.title = `图片 ID：${item.id}`;
    card.setAttribute("aria-pressed", "false");
    if (selected.has(item.id)) {
      card.classList.add("selected");
      card.setAttribute("aria-pressed", "true");
    }

    const thumb = document.createElement("div");
    thumb.className = "thumb";
    const image = document.createElement("img");
    image.src = `/api/v1/image/${encodeURIComponent(item.id)}`;
    image.alt = "游离图片";
    image.loading = "lazy";
    image.decoding = "async";
    image.addEventListener(
      "error",
      () => {
        thumb.classList.add("placeholder");
        thumb.replaceChildren(makePlaceholder("无法预览"));
      },
      { once: true },
    );
    thumb.appendChild(image);

    const meta = document.createElement("div");
    meta.className = "orphan-meta";

    const idLine = document.createElement("span");
    idLine.className = "orphan-id";
    idLine.textContent = shortId(item.id);
    idLine.title = item.id;

    const width = Number.isInteger(item.width) ? item.width : null;
    const height = Number.isInteger(item.height) ? item.height : null;
    const frames = Number.isInteger(item.frameCount) ? item.frameCount : null;
    const sizeLine = document.createElement("span");
    sizeLine.textContent = `${width ?? "?"} × ${height ?? "?"} · ${
      frames ?? "?"
    } 帧`;

    const dateLine = document.createElement("span");
    dateLine.textContent = formatDate(item.createdAt);

    meta.append(idLine, sizeLine, dateLine);

    const check = document.createElement("span");
    check.className = "orphan-check";
    check.textContent = "✓";
    check.setAttribute("aria-hidden", "true");

    card.append(thumb, meta, check);
    card.addEventListener("click", (event) => {
      if (event.ctrlKey || event.metaKey) {
        toggleSelection(item.id);
      } else {
        selectOnly(item.id);
      }
    });
    card.addEventListener("keydown", (event) => {
      if (event.key === "Enter" || event.key === " ") {
        event.preventDefault();
        toggleSelection(item.id);
      }
    });

    return card;
  }

  function appendItems(pageItems) {
    const fragment = document.createDocumentFragment();
    for (const item of pageItems) {
      if (cards.has(item.id)) {
        continue;
      }
      items.push(item);
      const card = createCard(item);
      cards.set(item.id, card);
      fragment.appendChild(card);
    }
    grid.appendChild(fragment);
  }

  async function fetchOrphans(offset, includeTotal, signal) {
    const data = await graphql(
      ORPHAN_QUERY,
      { limit: PAGE_SIZE, offset, includeTotal },
      signal,
    );
    return data.orphanImages;
  }

  function maybeLoadMore() {
    if (loading || !hasMore) {
      return;
    }
    const remaining =
      document.documentElement.scrollHeight -
      window.scrollY -
      window.innerHeight;
    if (remaining <= SCROLL_THRESHOLD) {
      loadNextPage();
    }
  }

  async function loadNextPage() {
    if (loading || !hasMore) {
      return;
    }
    loading = true;
    const version = loadVersion;
    const includeTotal = nextOffset === 0;
    setScrollStatus("加载中…");

    let succeeded = false;
    try {
      const page = await fetchOrphans(nextOffset, includeTotal, null);
      if (version !== loadVersion) {
        return;
      }
      appendItems(Array.isArray(page.items) ? page.items : []);
      nextOffset += Array.isArray(page.items) ? page.items.length : 0;
      hasMore = Boolean(page.hasMore);
      if (typeof page.total === "number") {
        total = page.total;
      }
      empty.hidden = items.length > 0;
      updateStatus();
      // 只清掉上一次的加载错误，保留提交成功之类的提示。
      if (notice.classList.contains("error")) {
        setNotice("");
      }
      succeeded = true;
    } catch (error) {
      if (version !== loadVersion) {
        return;
      }
      setNotice(`加载失败：${error.message}`, true);
    } finally {
      if (version === loadVersion) {
        loading = false;
        if (!succeeded) {
          setScrollStatus("");
        } else if (hasMore) {
          setScrollStatus("");
        } else {
          setScrollStatus(items.length > 0 ? "没有更多了" : "");
        }
        if (succeeded) {
          maybeLoadMore();
        }
      }
    }
  }

  async function resetAndLoad() {
    loadVersion += 1;
    loading = false;
    nextOffset = 0;
    hasMore = true;
    total = null;
    items.length = 0;
    cards.clear();
    selected.clear();
    grid.replaceChildren();
    empty.hidden = true;
    setNotice("");
    setScrollStatus("");
    updateStatus();
    updateAddButton();
    await loadNextPage();
  }

  function renderTags() {
    const fragment = document.createDocumentFragment();
    for (const tag of tags) {
      const chip = document.createElement("button");
      chip.type = "button";
      chip.className = "chip chip-filter";
      chip.textContent = tag.name;
      chip.title = `${tag.path}\n点击移除`;
      chip.addEventListener("click", () => {
        const index = tags.findIndex((item) => item.id === tag.id);
        if (index >= 0) {
          tags.splice(index, 1);
          renderTags();
        }
      });
      fragment.appendChild(chip);
    }
    tagStage.replaceChildren(fragment);
    tagStage.hidden = tags.length === 0;
  }

  async function addTagFromInput() {
    const normalized = TagSearch.trimPathSuffix(tagInput.value);
    if (normalized === "") {
      return;
    }
    showAddError("");

    const chosen = tagSuggestWidget.getChosen();
    let tag = null;
    if (chosen && normalized === chosen.path) {
      tag = chosen;
    } else {
      try {
        tag = await TagSearch.resolveTagPath(normalized, null);
      } catch (error) {
        showAddError(error.message || "标签解析失败");
        return;
      }
    }

    if (!tag) {
      showAddError(`找不到标签路径：${normalized}`);
      return;
    }

    if (!tags.some((item) => item.id === tag.id)) {
      tags.push({ id: tag.id, name: tag.name, path: tag.path });
    }
    renderTags();

    // 与主页一致：保留完整路径并补上 ::，方便继续往下补全。
    tagInput.value = `${tag.path}::`;
    tagSuggestWidget.clearChosen();
    tagSuggestWidget.close();
    tagInput.focus();
    tagInput.setSelectionRange(tagInput.value.length, tagInput.value.length);
  }

  function showAddError(text) {
    addError.textContent = text;
    addError.hidden = !text;
  }

  function currentAddMode() {
    const checked = addModeInputs.find((item) => item.checked);
    return checked ? checked.value : "artwork";
  }

  function updateAddMode() {
    const oneImage = currentAddMode() === "oneimg";
    addModeHint.textContent = oneImage
      ? "每个图片生成一个独立作品"
      : "所有选中图片合成一个作品";
    addTitleHint.textContent = oneImage ? "可选，留空则用实体 ID" : "必填";
    addTitle.placeholder = oneImage
      ? "留空则每个作品用实体 ID"
      : "作品标题";
    addPublishedField.hidden = oneImage;
  }

  function updateAddCount() {
    addCount.textContent = `已选择 ${selected.size} 张图片`;
  }

  function resetAddForm() {
    addForm.reset();
    tags.length = 0;
    renderTags();
    tagSuggestWidget.clearChosen();
    tagSuggestWidget.close();
    updateAddMode();
    showAddError("");
    updateAddCount();
  }

  function openAddDialog() {
    if (selected.size === 0) {
      return;
    }
    resetAddForm();
    addDialog.showModal();
    addTitle.focus();
  }

  async function requestAddArtwork(input) {
    const data = await graphql(ADD_MUTATION, { input });
    const taskId = data.addArtworkFromImages;
    if (!taskId) {
      throw new Error("服务端没有返回 taskId");
    }
    return taskId;
  }

  async function refreshAfterSubmit(beforeTotal) {
    if (beforeTotal === null || beforeTotal === undefined) {
      await delay(1200);
      resetAndLoad();
      return;
    }

    for (let attempt = 0; attempt < SCAN_POLL_ATTEMPTS; attempt += 1) {
      await delay(SCAN_POLL_INTERVAL);
      try {
        const page = await fetchOrphans(0, true, null);
        if (typeof page.total === "number" && page.total < beforeTotal) {
          // 等列表重新加载完再提示，否则加载成功会清空 notice。
          await resetAndLoad();
          setNotice("作品已创建，列表已刷新");
          return;
        }
      } catch {
        // 轮询失败继续重试。
      }
    }
    resetAndLoad();
  }

  function finishSubmit(count, submittedItems, failure) {
    const beforeTotal = total;
    addDialog.close();
    resetAddForm();
    clearSelection();
    addSubmit.disabled = false;
    addSubmit.textContent = "添加";

    if (failure) {
      setNotice(
        `已提交 ${count} 个作品，第 ${count + 1} 个失败：${failure.message}`,
        true,
      );
    } else {
      setNotice(`已提交 ${count} 个作品，正在刷新…`);
    }
    refreshAfterSubmit(beforeTotal);
  }

  async function submitAdd(event) {
    event.preventDefault();
    showAddError("");

    const mode = currentAddMode();
    const title = addTitle.value.trim();
    if (mode === "artwork" && title === "") {
      showAddError("请填写标题");
      addTitle.focus();
      return;
    }

    const chosenItems = selectedItems();
    if (chosenItems.length === 0) {
      showAddError("请先选择图片");
      return;
    }

    const published = addPublished.value.trim();
    if (
      mode === "artwork" &&
      published !== "" &&
      !TIMESTAMP_PATTERN.test(published)
    ) {
      showAddError("发布时间格式应为 RFC3339，例如 2026-04-05T06:07:08Z");
      addPublished.focus();
      return;
    }

    const description = addDescription.value.trim();
    const tagIds = tags.map((tag) => tag.id);
    const shared = {};
    if (description !== "") {
      shared.description = description;
    }
    if (tagIds.length > 0) {
      shared.tagIds = tagIds;
    }
    // 单图模式不写发布时间。
    if (mode === "artwork" && published !== "") {
      shared.publishedAt = published;
    }

    const rescan = addRescan.checked;
    addSubmit.disabled = true;
    addSubmit.textContent = "提交中…";

    try {
      if (mode === "oneimg") {
        let submitted = 0;
        let failure = null;
        for (const item of chosenItems) {
          addSubmit.textContent = `提交中… ${submitted + 1}/${
            chosenItems.length
          }`;
          try {
            await requestAddArtwork({
              imageIds: [item.id],
              title: title || item.id,
              ...shared,
              rescan,
            });
            submitted += 1;
          } catch (error) {
            failure = error;
            break;
          }
        }

        if (submitted === 0) {
          showAddError(failure ? failure.message : "添加失败");
          return;
        }
        finishSubmit(submitted, chosenItems, failure);
      } else {
        await requestAddArtwork({
          imageIds: chosenItems.map((item) => item.id),
          title,
          ...shared,
          rescan,
        });
        finishSubmit(chosenItems.length, chosenItems, null);
      }
    } catch (error) {
      showAddError(error.message || "添加失败");
    } finally {
      // 弹窗还开着说明提交失败，恢复按钮；成功时 finishSubmit 已重置。
      if (addDialog.open) {
        addSubmit.disabled = false;
        addSubmit.textContent = "添加";
      }
    }
  }

  for (const modeInput of addModeInputs) {
    modeInput.addEventListener("change", updateAddMode);
  }

  addButton.addEventListener("click", openAddDialog);
  addCancel.addEventListener("click", () => addDialog.close());
  addClose.addEventListener("click", () => addDialog.close());
  addDialog.addEventListener("click", (event) => {
    // 点到 dialog 本身说明点在遮罩上。
    if (event.target === addDialog) {
      addDialog.close();
    }
  });
  addForm.addEventListener("submit", submitAdd);

  window.addEventListener("scroll", maybeLoadMore, { passive: true });
  window.addEventListener("resize", maybeLoadMore);

  updateStatus();
  updateAddButton();
  resetAndLoad();
})();
