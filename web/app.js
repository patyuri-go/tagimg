(() => {
  "use strict";

  const PAGE_SIZE = 24;
  const MAX_ENTITIES = 20;
  // 扫描任务没有终态查询，靠轮询作品是否出现来判断完成。
  const SCAN_POLL_INTERVAL = 2500;
  const SCAN_POLL_ATTEMPTS = 24;
  const SEARCH_QUERY = `
    query SearchArtworks($filter: ArtworkFilter, $limit: Int!, $offset: Int!) {
      artworks(filter: $filter, limit: $limit, offset: $offset, includeTotal: true) {
        items {
          id
          title
          createdAt
          imageCount
          entityIds
          tags {
            id
            name
          }
        }
        hasMore
        total
      }
    }
  `;

  const ADD_MUTATION = `
    mutation AddArtwork($input: AddArtworkInput!) {
      addArtwork(input: $input)
    }
  `;

  const UUID_PATTERN =
    /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
  // 与服务端一致，只检查 RFC3339 的形状。
  const TIMESTAMP_PATTERN = /^\d{4}-\d{2}-\d{2}[Tt]\d{2}:\d{2}:\d{2}/;

  const form = document.getElementById("search-form");
  const input = document.getElementById("search-input");
  const results = document.getElementById("results");
  const status = document.getElementById("status");
  const loadMore = document.getElementById("load-more");

  const addButton = document.getElementById("add-button");
  const addDialog = document.getElementById("add-dialog");
  const addForm = document.getElementById("add-form");
  const addTitle = document.getElementById("add-title");
  const addEntities = document.getElementById("add-entities");
  const addDescription = document.getElementById("add-description");
  const addPublished = document.getElementById("add-published");
  const addError = document.getElementById("add-error");
  const addSubmit = document.getElementById("add-submit");
  const addCancel = document.getElementById("add-cancel");
  const addClose = document.getElementById("add-close");
  const entityCount = document.getElementById("entity-count");

  let offset = 0;
  let inFlight = null;
  let debounceTimer = 0;
  let scanToken = 0;

  function setStatus(text, kind) {
    status.textContent = text || "";
    status.hidden = !text;
    status.classList.toggle("error", kind === "error");
  }

  async function requestArtworks(title, fromOffset, signal) {
    const response = await fetch("/graphql", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        query: SEARCH_QUERY,
        variables: {
          filter: title ? { title } : null,
          limit: PAGE_SIZE,
          offset: fromOffset,
        },
      }),
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
          : `请求失败（HTTP ${response.status}）`;
      throw new Error(message);
    }
    if (!payload) {
      throw new Error("服务端返回的不是合法 JSON");
    }
    if (Array.isArray(payload.errors) && payload.errors.length > 0) {
      throw new Error(payload.errors.map((item) => item.message).join("；"));
    }

    const data = payload.data && payload.data.artworks;
    if (!data) {
      throw new Error("响应缺少 artworks 数据");
    }
    return data;
  }

  function formatDate(value) {
    if (!value) {
      return "";
    }
    // 服务端返回的是 UTC 秒级 RFC3339，这里只做展示，不转换时区。
    return value.replace("T", " ").replace("Z", "").slice(0, 16);
  }

  function createThumb(item) {
    const thumb = document.createElement("div");
    thumb.className = "thumb";

    const entityIds = Array.isArray(item.entityIds) ? item.entityIds : [];
    if (entityIds.length === 0) {
      thumb.classList.add("placeholder");
      thumb.textContent = "无预览";
      return thumb;
    }

    const image = document.createElement("img");
    image.loading = "lazy";
    image.decoding = "async";
    image.alt = item.title || "作品图片";
    // 缩略图接口还没实现，先直接取原图。
    image.src = `/api/v1/image/${encodeURIComponent(entityIds[0])}`;
    image.addEventListener(
      "error",
      () => {
        image.remove();
        thumb.classList.add("placeholder");
        thumb.textContent = "图片加载失败";
      },
      { once: true },
    );
    thumb.appendChild(image);
    return thumb;
  }

  function createCard(item) {
    const card = document.createElement("article");
    card.className = "card";

    const body = document.createElement("div");
    body.className = "card-body";

    const title = document.createElement("h2");
    title.className = "card-title";
    title.textContent = item.title || "（无标题）";
    title.title = item.title || "";

    const meta = document.createElement("div");
    meta.className = "meta";
    const time = document.createElement("span");
    time.textContent = formatDate(item.createdAt);
    const count = document.createElement("span");
    count.textContent = `${Number(item.imageCount) || 0} 张`;
    meta.append(time, count);

    body.append(title, meta);

    const tags = Array.isArray(item.tags) ? item.tags : [];
    if (tags.length > 0) {
      const list = document.createElement("div");
      list.className = "tags";
      for (const tag of tags.slice(0, 6)) {
        const chip = document.createElement("span");
        chip.className = "chip";
        chip.textContent = tag.name;
        chip.title = tag.name;
        list.appendChild(chip);
      }
      if (tags.length > 6) {
        const extra = document.createElement("span");
        extra.className = "chip chip-more";
        extra.textContent = `+${tags.length - 6}`;
        list.appendChild(extra);
      }
      body.appendChild(list);
    }

    card.append(createThumb(item), body);
    return card;
  }

  function render(items, append) {
    if (!append) {
      results.replaceChildren();
    }
    const fragment = document.createDocumentFragment();
    for (const item of items) {
      fragment.appendChild(createCard(item));
    }
    results.appendChild(fragment);
  }

  async function search(reset) {
    const title = input.value.trim();

    if (reset) {
      offset = 0;
      results.replaceChildren();
    }

    if (inFlight) {
      inFlight.abort();
    }
    const controller = new AbortController();
    inFlight = controller;

    loadMore.hidden = true;
    setStatus("搜索中…");

    try {
      const data = await requestArtworks(title, offset, controller.signal);
      if (inFlight !== controller) {
        return;
      }

      if (data.items.length === 0 && offset === 0) {
        setStatus(title ? `没有匹配“${title}”的作品` : "还没有作品");
      } else if (typeof data.total === "number") {
        setStatus(`共 ${data.total} 个作品`);
      } else {
        setStatus(`已加载 ${offset + data.items.length} 个作品`);
      }

      render(data.items, offset > 0);
      offset += data.items.length;
      loadMore.hidden = !data.hasMore;
    } catch (error) {
      if (error.name === "AbortError" || inFlight !== controller) {
        return;
      }
      setStatus(error.message || "请求失败", "error");
    } finally {
      if (inFlight === controller) {
        inFlight = null;
      }
    }
  }

  function delay(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
  }

  function showAddError(message) {
    addError.textContent = message || "";
    addError.hidden = !message;
  }

  function updateEntityCount() {
    const count = addEntities.value
      .split(/\r?\n/)
      .filter((line) => line.trim() !== "").length;
    entityCount.textContent =
      count > 0 ? `已输入 ${count} 个实体 ID` : "";
    entityCount.classList.toggle("error", count > MAX_ENTITIES);
  }

  // 一行一个实体 id；返回去空白后的列表，或第一个错误。
  function parseEntityIds() {
    const lines = addEntities.value.split(/\r?\n/);
    const ids = [];
    const seen = new Set();
    for (let index = 0; index < lines.length; index += 1) {
      const value = lines[index].trim();
      if (value === "") {
        continue;
      }
      if (!UUID_PATTERN.test(value)) {
        return { error: `第 ${index + 1} 行不是合法的 UUID` };
      }
      const key = value.toLowerCase();
      if (seen.has(key)) {
        return { error: `实体 ID 重复：${value}` };
      }
      seen.add(key);
      ids.push(value);
    }
    if (ids.length === 0) {
      return { error: "请至少输入一个实体 ID" };
    }
    if (ids.length > MAX_ENTITIES) {
      return { error: `最多 ${MAX_ENTITIES} 个实体 ID` };
    }
    return { ids };
  }

  async function submitAdd(event) {
    event.preventDefault();
    showAddError("");

    const title = addTitle.value.trim();
    if (title === "") {
      showAddError("请填写标题");
      addTitle.focus();
      return;
    }

    const parsed = parseEntityIds();
    if (parsed.error) {
      showAddError(parsed.error);
      addEntities.focus();
      return;
    }

    const published = addPublished.value.trim();
    if (published !== "" && !TIMESTAMP_PATTERN.test(published)) {
      showAddError("发布时间格式应为 RFC3339，例如 2026-04-05T06:07:08Z");
      addPublished.focus();
      return;
    }

    const inputVariables = { entityIds: parsed.ids, title };
    const description = addDescription.value.trim();
    if (description !== "") {
      inputVariables.description = description;
    }
    if (published !== "") {
      inputVariables.publishedAt = published;
    }

    addSubmit.disabled = true;
    addSubmit.textContent = "提交中…";
    try {
      const response = await fetch("/graphql", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          query: ADD_MUTATION,
          variables: { input: inputVariables },
        }),
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
            : `请求失败（HTTP ${response.status}）`;
        throw new Error(message);
      }
      if (Array.isArray(payload?.errors) && payload.errors.length > 0) {
        throw new Error(payload.errors[0].message || "添加失败");
      }

      const taskId = payload?.data?.addArtwork;
      if (!taskId) {
        throw new Error("服务端没有返回 taskId");
      }

      addDialog.close();
      addForm.reset();
      updateEntityCount();
      setStatus("已提交扫描任务，正在处理…");
      watchScan(parsed.ids);
    } catch (error) {
      showAddError(error.message || "添加失败");
    } finally {
      addSubmit.disabled = false;
      addSubmit.textContent = "添加";
    }
  }

  async function findArtworkByEntities(entityIds) {
    const response = await fetch("/graphql", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        query: SEARCH_QUERY,
        variables: { filter: { entityIds }, limit: 1, offset: 0 },
      }),
    });
    const payload = await response.json();
    if (!response.ok || (Array.isArray(payload.errors) && payload.errors.length)) {
      return null;
    }
    const items = payload?.data?.artworks?.items;
    return Array.isArray(items) && items.length > 0 ? items[0] : null;
  }

  // 扫描成功与否只能看作品有没有出现；失败原因只在服务端日志里。
  async function watchScan(entityIds) {
    const token = ++scanToken;
    for (let attempt = 0; attempt < SCAN_POLL_ATTEMPTS; attempt += 1) {
      await delay(SCAN_POLL_INTERVAL);
      if (token !== scanToken) {
        return;
      }
      const found = await findArtworkByEntities(entityIds).catch(() => null);
      if (found) {
        await search(true);
        if (token === scanToken && !status.classList.contains("error")) {
          setStatus("扫描完成，作品已入库");
        }
        return;
      }
    }
    if (token === scanToken) {
      setStatus("扫描仍在进行，稍后可点搜索刷新", "error");
    }
  }

  form.addEventListener("submit", (event) => {
    event.preventDefault();
    clearTimeout(debounceTimer);
    search(true);
  });

  input.addEventListener("input", () => {
    clearTimeout(debounceTimer);
    debounceTimer = setTimeout(() => search(true), 300);
  });

  loadMore.addEventListener("click", () => {
    search(false);
  });

  addButton.addEventListener("click", () => {
    showAddError("");
    addDialog.showModal();
    addTitle.focus();
  });

  addCancel.addEventListener("click", () => addDialog.close());
  addClose.addEventListener("click", () => addDialog.close());

  addDialog.addEventListener("click", (event) => {
    // 点到 dialog 本身说明点在遮罩上。
    if (event.target === addDialog) {
      addDialog.close();
    }
  });

  addForm.addEventListener("submit", submitAdd);
  addEntities.addEventListener("input", updateEntityCount);

  // 初次打开展示最近的 24 个作品。
  search(true);
})();
