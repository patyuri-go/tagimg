(() => {
  "use strict";

  const PAGE_SIZE = 24;
  const MAX_ENTITIES = 200;
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
          tagPaths {
            id
            path
          }
          tags {
            id
            name
            category
            aliases
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

  // 单图模式提交后只轮询总数，避免每轮把所有作品字段都取回来。
  const COUNT_QUERY = `
    query CountArtworks($filter: ArtworkFilter) {
      artworks(filter: $filter, limit: 1, offset: 0, includeTotal: true) {
        total
      }
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
  const tagInput = document.getElementById("tag-input");
  const tagSuggest = document.getElementById("tag-suggest");
  const tagStaging = document.getElementById("tag-staging");
  const includeStageRow = document.getElementById("include-stage-row");
  const includeStage = document.getElementById("include-stage");
  const excludeStageRow = document.getElementById("exclude-stage-row");
  const excludeStage = document.getElementById("exclude-stage");

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
  const addModeInputs = Array.from(
    document.querySelectorAll('input[name="add-mode"]'),
  );
  const addModeHint = document.getElementById("add-mode-hint");
  const addTitleHint = document.getElementById("add-title-hint");
  const addPublishedField = document.getElementById("add-published-field");
  const addTagInput = document.getElementById("add-tag-input");
  const addTagSuggest = document.getElementById("add-tag-suggest");
  const addTagStage = document.getElementById("add-tag-stage");

  let offset = 0;
  let inFlight = null;
  let debounceTimer = 0;
  let scanToken = 0;

  // 标签输入联想与路径解析统一放在 tag-search.js，主搜索框和弹窗各建一个实例。
  const tagSuggestWidget = TagSearch.create({
    input: tagInput,
    list: tagSuggest,
    idPrefix: "tag",
    onEnter: () => addTagFromInput("include"),
    onShiftEnter: () => addTagFromInput("exclude"),
  });
  const addTagSuggestWidget = TagSearch.create({
    input: addTagInput,
    list: addTagSuggest,
    idPrefix: "add-tag",
    onEnter: () => addTagFromDialogInput(),
  });

  // 暂存区，元素为 { id, name, path }。
  const includeTags = [];
  const excludeTags = [];
  const addTags = [];

  function setStatus(text, kind) {
    status.textContent = text || "";
    status.hidden = !text;
    status.classList.toggle("error", kind === "error");
  }

  function renderAddTags() {
    const fragment = document.createDocumentFragment();
    for (const tag of addTags) {
      const chip = document.createElement("button");
      chip.type = "button";
      chip.className = "chip chip-filter";
      chip.textContent = tag.name;
      chip.title = `${tag.path}\n点击移除`;
      chip.addEventListener("click", () => {
        const index = addTags.findIndex((item) => item.id === tag.id);
        if (index >= 0) {
          addTags.splice(index, 1);
          renderAddTags();
        }
      });
      fragment.appendChild(chip);
    }
    addTagStage.replaceChildren(fragment);
    addTagStage.hidden = addTags.length === 0;
  }

  async function addTagFromDialogInput() {
    const normalized = TagSearch.trimPathSuffix(addTagInput.value);
    if (normalized === "") {
      return;
    }
    showAddError("");

    const chosen = addTagSuggestWidget.getChosen();
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

    if (!addTags.some((item) => item.id === tag.id)) {
      addTags.push({ id: tag.id, name: tag.name, path: tag.path });
    }
    renderAddTags();

    // 与主搜索框一致：保留完整路径并补上 ::，方便继续补全。
    addTagInput.value = `${tag.path}::`;
    addTagSuggestWidget.clearChosen();
    addTagSuggestWidget.close();
    addTagInput.focus();
    addTagInput.setSelectionRange(
      addTagInput.value.length,
      addTagInput.value.length,
    );
  }

  // 暂存区：只显示标签名，点击移除；包含与不包含互斥，后加覆盖。
  function addStageTag(tag, kind) {
    const target = kind === "exclude" ? excludeTags : includeTags;
    const other = kind === "exclude" ? includeTags : excludeTags;
    const otherIndex = other.findIndex((item) => item.id === tag.id);
    if (otherIndex >= 0) {
      other.splice(otherIndex, 1);
    }
    if (!target.some((item) => item.id === tag.id)) {
      target.push(tag);
    }
    renderStaging();
    syncTagsToUrl();
    search(true);
  }

  function removeStageTag(id, kind) {
    const list = kind === "exclude" ? excludeTags : includeTags;
    const index = list.findIndex((item) => item.id === id);
    if (index < 0) {
      return;
    }
    list.splice(index, 1);
    renderStaging();
    syncTagsToUrl();
    search(true);
  }

  function fillStage(listEl, tags, kind) {
    const fragment = document.createDocumentFragment();
    for (const tag of tags) {
      const chip = document.createElement("button");
      chip.type = "button";
      chip.className = "chip chip-filter";
      chip.textContent = tag.name;
      chip.title = `${tag.path}\n点击移除`;
      chip.addEventListener("click", () => removeStageTag(tag.id, kind));
      fragment.appendChild(chip);
    }
    listEl.replaceChildren(fragment);
  }

  function renderStaging() {
    fillStage(includeStage, includeTags, "include");
    fillStage(excludeStage, excludeTags, "exclude");
    includeStageRow.hidden = includeTags.length === 0;
    excludeStageRow.hidden = excludeTags.length === 0;
    tagStaging.hidden = includeTags.length === 0 && excludeTags.length === 0;
  }

  // 从 URL 还原包含标签；详情页的标签链接带 tag/name/path 三个参数。
  function restoreTagsFromUrl() {
    const params = new URLSearchParams(window.location.search);
    const ids = params.getAll("tag");
    const names = params.getAll("name");
    const paths = params.getAll("path");
    for (let index = 0; index < ids.length; index += 1) {
      const id = (ids[index] || "").trim().toLowerCase();
      if (!UUID_PATTERN.test(id)) {
        continue;
      }
      const path = paths[index] || "";
      const name =
        (names[index] || "").trim() ||
        (path ? path.split("::").pop() : "") ||
        "标签";
      if (!includeTags.some((tag) => tag.id === id)) {
        includeTags.push({ id, name, path });
      }
    }
  }

  // 让 URL 始终反映包含暂存区，刷新或分享后仍能还原。
  function syncTagsToUrl() {
    const params = new URLSearchParams();
    for (const tag of includeTags) {
      params.append("tag", tag.id);
      params.append("name", tag.name);
      if (tag.path) {
        params.append("path", tag.path);
      }
    }
    const query = params.toString();
    const url = query
      ? `${window.location.pathname}?${query}`
      : window.location.pathname;
    window.history.replaceState(null, "", url);
  }

  // 回车：把输入框里的完整路径加入包含或排除暂存区。
  async function addTagFromInput(kind) {
    const normalized = TagSearch.trimPathSuffix(tagInput.value);
    if (normalized === "") {
      return;
    }

    const chosen = tagSuggestWidget.getChosen();
    let tag = null;
    if (chosen && normalized === chosen.path) {
      tag = chosen;
    } else {
      try {
        tag = await TagSearch.resolveTagPath(normalized, null);
      } catch (error) {
        setStatus(error.message || "标签解析失败", "error");
        return;
      }
    }

    if (!tag) {
      setStatus(`找不到标签路径：${normalized}`, "error");
      return;
    }

    // 保留完整路径并补上 ::，方便继续在它下面补全。
    tagInput.value = `${tag.path}::`;
    tagSuggestWidget.clearChosen();
    tagSuggestWidget.close();
    tagInput.focus();
    tagInput.setSelectionRange(tagInput.value.length, tagInput.value.length);
    addStageTag(tag, kind);
  }

  // 标题 + 暂存区标签共同组成作品过滤条件。
  function currentFilter() {
    const filter = {};
    const title = input.value.trim();
    if (title) {
      filter.title = title;
    }
    if (includeTags.length > 0) {
      filter.includeTagIds = includeTags.map((tag) => tag.id);
      // 包含暂存区里的标签必须全部命中。
      filter.includeMode = "ALL";
    }
    if (excludeTags.length > 0) {
      filter.excludeTagIds = excludeTags.map((tag) => tag.id);
    }
    return Object.keys(filter).length > 0 ? filter : null;
  }

  async function requestArtworks(filter, fromOffset, signal) {
    const response = await fetch("/graphql", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        query: SEARCH_QUERY,
        variables: {
          filter,
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

  // 标签悬停提示：第一个别名、完整路径、直接父标签名。
  function tag_tooltip(tag, path) {
    const lines = [];
    const aliases = Array.isArray(tag.aliases) ? tag.aliases : [];
    if (aliases.length > 0) {
      lines.push(`别名：${aliases[0]}`);
    }
    if (path) {
      lines.push(`路径：${path}`);
    }
    if (tag.category) {
      lines.push(`分类：${tag.category}`);
    }
    return lines.join("\n");
  }

  function createCard(item) {
    // 整张卡片就是详情页链接，语义上用 a。
    const card = document.createElement("a");
    card.className = "card";
    card.href = `/artwork.html?id=${encodeURIComponent(item.id)}`;

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
      // 悬停提示要显示完整路径，先按标签 id 建索引。
      const pathById = new Map();
      for (const entry of Array.isArray(item.tagPaths) ? item.tagPaths : []) {
        pathById.set(entry.id, entry.path);
      }

      const list = document.createElement("div");
      list.className = "tags";
      for (const tag of tags.slice(0, 6)) {
        const chip = document.createElement("span");
        chip.className = "chip";
        chip.textContent = tag.name;
        chip.title = tag_tooltip(tag, pathById.get(tag.id));
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
    const filter = currentFilter();

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
      const data = await requestArtworks(filter, offset, controller.signal);
      if (inFlight !== controller) {
        return;
      }

      if (data.items.length === 0 && offset === 0) {
        setStatus(filter ? "没有符合条件的作品" : "还没有作品");
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

  function currentAddMode() {
    const checked = addModeInputs.find((item) => item.checked);
    return checked ? checked.value : "artwork";
  }

  function updateAddMode() {
    const oneImage = currentAddMode() === "oneimg";
    addModeHint.textContent = oneImage
      ? "每个实体生成一个独立作品"
      : "所有实体合成一个作品";
    addTitleHint.textContent = oneImage ? "可选，留空则用实体 ID" : "必填";
    addTitle.placeholder = oneImage ? "留空则每个作品用实体 ID" : "作品标题";
    addPublishedField.hidden = oneImage;
  }

  // 重置弹窗里除输入框以外的选择状态。
  function resetAddForm() {
    addForm.reset();
    addTags.length = 0;
    renderAddTags();
    addTagSuggestWidget.clearChosen();
    addTagSuggestWidget.close();
    updateEntityCount();
    updateAddMode();
  }

  async function requestAddArtwork(inputVariables) {
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
    return taskId;
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

    const parsed = parseEntityIds();
    if (parsed.error) {
      showAddError(parsed.error);
      addEntities.focus();
      return;
    }

    const published = addPublished.value.trim();
    if (mode === "artwork" && published !== "" && !TIMESTAMP_PATTERN.test(published)) {
      showAddError("发布时间格式应为 RFC3339，例如 2026-04-05T06:07:08Z");
      addPublished.focus();
      return;
    }

    const description = addDescription.value.trim();
    const tagIds = addTags.map((tag) => tag.id);
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

    addSubmit.disabled = true;
    addSubmit.textContent = "提交中…";
    try {
      if (mode === "oneimg") {
        const submitted = [];
        let failure = null;
        for (let index = 0; index < parsed.ids.length; index += 1) {
          const entityId = parsed.ids[index];
          addSubmit.textContent = `提交中… ${index + 1}/${parsed.ids.length}`;
          const input = {
            entityIds: [entityId],
            title: title || entityId,
            ...shared,
          };
          try {
            await requestAddArtwork(input);
            submitted.push(entityId);
          } catch (error) {
            failure = error;
            break;
          }
        }

        if (submitted.length === 0) {
          showAddError(failure ? failure.message : "添加失败");
          return;
        }

        addDialog.close();
        resetAddForm();
        if (failure) {
          setStatus(
            `已提交 ${submitted.length} 个，后续提交失败：${failure.message}`,
            "error",
          );
        } else {
          setStatus(`已提交 ${submitted.length} 个扫描任务，正在处理…`);
        }
        watchOneImage(submitted);
        return;
      }

      await requestAddArtwork({
        entityIds: parsed.ids,
        title,
        ...shared,
      });

      addDialog.close();
      resetAddForm();
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

  async function countArtworksByEntities(entityIds) {
    const response = await fetch("/graphql", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        query: COUNT_QUERY,
        variables: { filter: { entityIds } },
      }),
    });
    const payload = await response.json();
    if (!response.ok || (Array.isArray(payload.errors) && payload.errors.length)) {
      return 0;
    }
    return Number(payload?.data?.artworks?.total) || 0;
  }

  // 单图模式每个实体是一个独立任务，等它们全部出现再刷新列表。
  async function watchOneImage(entityIds) {
    const token = ++scanToken;
    for (let attempt = 0; attempt < SCAN_POLL_ATTEMPTS; attempt += 1) {
      await delay(SCAN_POLL_INTERVAL);
      if (token !== scanToken) {
        return;
      }
      const total = await countArtworksByEntities(entityIds).catch(() => 0);
      if (token !== scanToken) {
        return;
      }
      if (total >= entityIds.length) {
        await search(true);
        if (token === scanToken && !status.classList.contains("error")) {
          setStatus("扫描完成，作品已入库");
        }
        return;
      }
      if (total > 0) {
        setStatus(`扫描中… 已入库 ${total}/${entityIds.length}`);
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

  for (const modeInput of addModeInputs) {
    modeInput.addEventListener("change", updateAddMode);
  }

  loadMore.addEventListener("click", () => {
    search(false);
  });

  addButton.addEventListener("click", () => {
    showAddError("");
    updateAddMode();
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

  // 初次打开展示最近的 24 个作品；URL 带标签时先还原包含条件。
  restoreTagsFromUrl();
  renderStaging();
  renderAddTags();
  updateAddMode();
  search(true);
})();
