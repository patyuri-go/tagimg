(() => {
  "use strict";

  const PAGE_SIZE = 24;
  const MAX_ENTITIES = 200;
  // 扫描任务没有终态查询，靠轮询作品是否出现来判断完成。
  const SCAN_POLL_INTERVAL = 2500;
  const SCAN_POLL_ATTEMPTS = 24;
  // 悬停放大预览：指针静止多久触发、离开卡片后多久失去「秒开」资格。
  const HOVER_PREVIEW_DELAY = 1500;
  const HOVER_PREVIEW_WARM = 800;
  const SEARCH_QUERY = `
    query SearchArtworks(
      $filter: ArtworkFilter
      $limit: Int!
      $offset: Int!
      $orderBy: ArtworkOrderField
      $sortText: String
    ) {
      artworks(
        filter: $filter
        limit: $limit
        offset: $offset
        orderBy: $orderBy
        sortText: $sortText
        includeTotal: true
      ) {
        items {
          id
          title
          createdAt
          imageCount
          imageIds
          sortScore
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
  const hoverPreview = document.getElementById("hover-preview");
  const status = document.getElementById("status");
  const layoutTools = document.getElementById("layout-tools");
  const layoutToggle = document.getElementById("layout-toggle");
  const layoutPanel = document.getElementById("layout-panel");
  const layoutClose = document.getElementById("layout-close");
  const layoutColGap = document.getElementById("layout-col-gap");
  const layoutColGapValue = document.getElementById("layout-col-gap-value");
  const layoutRowGap = document.getElementById("layout-row-gap");
  const layoutRowGapValue = document.getElementById("layout-row-gap-value");
  const layoutRadius = document.getElementById("layout-radius");
  const layoutRadiusValue = document.getElementById("layout-radius-value");
  const layoutModeVertical = document.getElementById("layout-mode-vertical");
  const layoutModeHorizontal = document.getElementById("layout-mode-horizontal");
  const layoutColumnsField = document.getElementById("layout-columns-field");
  const layoutColumns = document.getElementById("layout-columns");
  const layoutColumnsValue = document.getElementById("layout-columns-value");
  const layoutHeightField = document.getElementById("layout-height-field");
  const layoutHeight = document.getElementById("layout-height");
  const layoutHeightValue = document.getElementById("layout-height-value");
  const tagInput = document.getElementById("tag-input");
  const tagSuggest = document.getElementById("tag-suggest");
  const sortSearch = document.getElementById("sort-search");
  const sortInput = document.getElementById("sort-input");
  const sortClear = document.getElementById("sort-clear");
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

  // 无限滚动提前量：滚动到距底部这个距离时开始下一页。
  const LOAD_MORE_MARGIN = 400;

  let offset = 0;
  let inFlight = null;
  let debounceTimer = 0;
  let scanToken = 0;
  // 非空时按语义相似度排序，配合 GraphQL 的 orderBy: SIMILARITY。
  let sortText = "";
  let hasMore = false;
  let columns = [];
  let allCards = [];
  let resizeTimer = 0;
  const imageSizeCache = new Map();

  // 排版设置只影响卡片排列和外观，不改动后端数据。
  const layoutSettings = {
    mode: "vertical",
    colGap: 18,
    rowGap: 18,
    radius: 14,
    columns: 4,
    height: 240,
  };

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
          // 未设置排序文本时传 null，交给 schema 的默认值 CREATED_AT。
          orderBy: sortText ? "SIMILARITY" : null,
          sortText: sortText || null,
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

  function imageStreamUrl(imageId, size) {
    const url = `/api/v1/image/${encodeURIComponent(imageId)}`;
    return size ? `${url}?size=${encodeURIComponent(size)}` : url;
  }

  // 先离屏探测图片尺寸，卡片插入前就确定宽高比，避免图片加载后把下方内容顶走。
  function probeImageSize(imageId) {
    const cached = imageSizeCache.get(imageId);
    if (cached) {
      return cached;
    }

    const promise = new Promise((resolve) => {
      const image = new Image();
      let settled = false;
      let timeout = 0;
      const finish = (size) => {
        if (settled) {
          return;
        }
        settled = true;
        clearTimeout(timeout);
        image.onload = null;
        image.onerror = null;
        resolve(size);
      };
      timeout = setTimeout(() => finish(null), 30000);
      image.onload = () => {
        if (image.naturalWidth > 0 && image.naturalHeight > 0) {
          finish({ width: image.naturalWidth, height: image.naturalHeight });
        } else {
          finish(null);
        }
      };
      image.onerror = () => finish(null);
      // 等比缩放不会改变宽高比，用 thumb 探测足够，且比原图省带宽。
      image.src = imageStreamUrl(imageId, "thumb");
    });
    imageSizeCache.set(imageId, promise);
    return promise;
  }

  async function resolveThumbInfo(item) {
    const imageIds = Array.isArray(item.imageIds) ? item.imageIds : [];
    if (imageIds.length === 0) {
      return { kind: "empty" };
    }
    const imageId = imageIds[0];
    const size = await probeImageSize(imageId);
    if (!size) {
      return { kind: "error" };
    }
    return { kind: "image", imageId, ...size };
  }

  // 用图片 ID 的宽度和高度先把缩略图比例固定下来。
  function createThumb(item, thumbInfo) {
    const thumb = document.createElement("div");
    thumb.className = "thumb";

    if (thumbInfo.kind === "empty") {
      thumb.classList.add("placeholder");
      thumb.textContent = "无预览";
      return thumb;
    }
    if (thumbInfo.kind === "error") {
      thumb.classList.add("placeholder");
      thumb.textContent = "图片加载失败";
      return thumb;
    }

    thumb.classList.add("loaded");
    thumb.style.aspectRatio = `${thumbInfo.width} / ${thumbInfo.height}`;

    const image = document.createElement("img");
    image.loading = "lazy";
    image.decoding = "async";
    image.alt = item.title || "作品图片";
    image.addEventListener(
      "error",
      () => {
        // 保留占位比例，避免图片失败时下方卡片跳动。
        image.remove();
        thumb.classList.add("placeholder");
        thumb.textContent = "图片加载失败";
      },
      { once: true },
    );
    image.src = imageStreamUrl(thumbInfo.imageId, "small");
    // 悬停放大预览靠这些属性还原比例，避免重新探测或请求原图。
    image.dataset.imageId = thumbInfo.imageId;
    image.dataset.imageWidth = String(thumbInfo.width);
    image.dataset.imageHeight = String(thumbInfo.height);
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

  function createCard(item, thumbInfo) {
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

    const ratio =
      thumbInfo.kind === "image" &&
      thumbInfo.width > 0 &&
      thumbInfo.height > 0
        ? thumbInfo.width / thumbInfo.height
        : 3 / 2;
    card.dataset.aspectRatio = String(ratio);

    const thumb = createThumb(item, thumbInfo);
    // 只有按文字排序时才标分数，普通浏览不打扰。
    if (sortText) {
      const badge = document.createElement("span");
      badge.className = "score-badge";
      if (typeof item.sortScore === "number" && Number.isFinite(item.sortScore)) {
        badge.textContent = item.sortScore.toFixed(3);
        badge.classList.add("score-badge-hit");
        badge.title = `与「${sortText}」的相似度 ${item.sortScore.toFixed(4)}`;
      } else {
        badge.textContent = "无向量";
        badge.classList.add("score-badge-empty");
        badge.title = "这个作品还没有图片向量，无法参与相似度排序，已排在最后";
      }
      thumb.appendChild(badge);
    }

    card.append(thumb, body);
    return card;
  }

  function desiredColumnCount() {
    return Math.max(1, Math.min(8, Math.round(layoutSettings.columns)));
  }

  // 追加新卡片时只放到当前最短的列，已有卡片不会被重新洗牌。
  function shortestColumn() {
    let target = columns[0];
    let minHeight = target.offsetHeight;
    for (let index = 1; index < columns.length; index += 1) {
      const height = columns[index].offsetHeight;
      if (height < minHeight) {
        minHeight = height;
        target = columns[index];
      }
    }
    return target;
  }

  // 横向模式按宽高比分配 flex 宽度，行高由设置项控制。
  function applyCardLayout(card) {
    if (layoutSettings.mode !== "horizontal") {
      card.style.removeProperty("flex-basis");
      card.style.removeProperty("flex-grow");
      card.style.removeProperty("flex-shrink");
      return;
    }
    const ratio = Number(card.dataset.aspectRatio) || 1.5;
    card.style.flexBasis = `${ratio * layoutSettings.height}px`;
    card.style.flexGrow = String(ratio);
    card.style.flexShrink = "1";
  }

  function placeCard(card) {
    applyCardLayout(card);
    if (layoutSettings.mode === "horizontal") {
      results.appendChild(card);
      return;
    }
    shortestColumn().appendChild(card);
  }

  // 间距和圆角只改 CSS 变量；模式、列数变化才重建容器。
  function applyLayoutSettings() {
    results.dataset.layout = layoutSettings.mode;
    results.style.setProperty("--col-gap", `${layoutSettings.colGap}px`);
    results.style.setProperty("--row-gap", `${layoutSettings.rowGap}px`);
    results.style.setProperty("--img-radius", `${layoutSettings.radius}px`);
    for (const card of allCards) {
      applyCardLayout(card);
    }
  }

  function rebuildLayout() {
    results.replaceChildren();
    columns = [];
    if (layoutSettings.mode === "vertical") {
      const count = desiredColumnCount();
      for (let index = 0; index < count; index += 1) {
        const column = document.createElement("div");
        column.className = "masonry-column";
        columns.push(column);
      }
      results.append(...columns);
    }
    applyLayoutSettings();
    for (const card of allCards) {
      placeCard(card);
    }
  }

  function layoutNeedsRebuild() {
    if (layoutSettings.mode === "horizontal") {
      return results.dataset.layout !== "horizontal" || columns.length !== 0;
    }
    return (
      results.dataset.layout !== "vertical" ||
      columns.length !== desiredColumnCount()
    );
  }

  function relayout() {
    if (layoutNeedsRebuild()) {
      rebuildLayout();
    }
  }

  // 按后端顺序渐进插入：只有当前页开头连续就绪的卡片才能出现。
  // 例如顺序 1/2/3/4，2 和 4 先加载不插入；1 就绪后插入 1、2，4 仍等待 3。
  async function prepareCards(items, signal, onCard) {
    const entries = items.map((item) => ({
      item,
      card: null,
      ready: false,
    }));
    let next = 0;

    const flushReadyPrefix = () => {
      if (signal.aborted) {
        return;
      }
      while (next < entries.length && entries[next].ready) {
        onCard(entries[next].card);
        next += 1;
      }
    };

    await Promise.all(
      entries.map(async (entry) => {
        const thumbInfo = await resolveThumbInfo(entry.item);
        if (signal.aborted) {
          return;
        }
        entry.card = createCard(entry.item, thumbInfo);
        entry.ready = true;
        // 单线程事件循环里同步推进，保证不会把后面的卡片插到缺口前。
        flushReadyPrefix();
      }),
    );

    return next;
  }

  function nearBottom() {
    const scrollBottom = window.innerHeight + window.scrollY;
    const pageHeight = Math.max(
      document.documentElement.scrollHeight,
      document.body.scrollHeight,
    );
    return scrollBottom + LOAD_MORE_MARGIN >= pageHeight;
  }

  function maybeLoadMore() {
    if (!hasMore || inFlight || !nearBottom()) {
      return;
    }
    search(false);
  }

  async function search(reset) {
    const filter = currentFilter();

    if (reset) {
      offset = 0;
      hasMore = false;
      allCards = [];
      columns = [];
      results.replaceChildren();
    }

    if (inFlight) {
      inFlight.abort();
    }
    const controller = new AbortController();
    inFlight = controller;

    setStatus(reset ? "搜索中…" : "加载中…");

    try {
      const data = await requestArtworks(filter, offset, controller.signal);
      if (inFlight !== controller) {
        return;
      }

      // 第一页还没有列时先建列；后续分页只追加，不重排旧卡片。
      if (data.items.length > 0 && layoutNeedsRebuild()) {
        rebuildLayout();
      }

      const inserted = await prepareCards(
        data.items,
        controller.signal,
        (card) => {
          allCards.push(card);
          placeCard(card);
        },
      );
      if (controller.signal.aborted || inFlight !== controller) {
        return;
      }
      // 请求被替换或中途失败时，当前页没有完整插入，不能推进 offset。
      if (inserted !== data.items.length) {
        return;
      }

      offset += data.items.length;
      hasMore = Boolean(data.hasMore) && data.items.length > 0;

      if (offset === 0) {
        setStatus(filter ? "没有符合条件的作品" : "还没有作品");
      } else if (reset && typeof data.total === "number") {
        setStatus(`共 ${data.total} 个作品`);
      } else {
        setStatus(`已加载 ${offset} 个作品`);
      }

      // 当前请求的 inFlight 要到 finally 才释放，延后一拍再判断是否需要续页。
      setTimeout(maybeLoadMore, 0);
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

  // 文字排序：输入描述后按回车才请求，避免每敲一个字都让 sidecar 编码一次。
  function syncSortInput() {
    const active = sortText.length > 0;
    sortSearch.classList.toggle("is-active", active);
    sortClear.hidden = !active;
  }

  function applySortText(value) {
    const next = value.trim();
    sortInput.value = next;
    if (next === sortText) {
      syncSortInput();
      return;
    }
    sortText = next;
    syncSortInput();
    clearTimeout(debounceTimer);
    search(true);
  }

  sortInput.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      event.preventDefault();
      applySortText(sortInput.value);
    }
  });

  sortInput.addEventListener("input", () => {
    // 清空输入框（包括 type=search 自带的清除按钮）时立刻取消排序。
    if (!sortInput.value) {
      applySortText("");
      return;
    }
    sortClear.hidden = false;
  });

  sortClear.addEventListener("click", () => {
    applySortText("");
    sortInput.focus();
  });

  syncSortInput();

  for (const modeInput of addModeInputs) {
    modeInput.addEventListener("change", updateAddMode);
  }

  function syncLayoutSettings() {
    const vertical = layoutSettings.mode === "vertical";
    layoutColGap.value = String(layoutSettings.colGap);
    layoutColGapValue.textContent = String(layoutSettings.colGap);
    layoutRowGap.value = String(layoutSettings.rowGap);
    layoutRowGapValue.textContent = String(layoutSettings.rowGap);
    layoutRadius.value = String(layoutSettings.radius);
    layoutRadiusValue.textContent = String(layoutSettings.radius);
    layoutColumns.value = String(layoutSettings.columns);
    layoutColumnsValue.textContent = String(layoutSettings.columns);
    layoutHeight.value = String(layoutSettings.height);
    layoutHeightValue.textContent = String(layoutSettings.height);
    layoutModeVertical.classList.toggle("active", vertical);
    layoutModeHorizontal.classList.toggle("active", !vertical);
    layoutModeVertical.setAttribute("aria-pressed", String(vertical));
    layoutModeHorizontal.setAttribute("aria-pressed", String(!vertical));
    layoutColumnsField.hidden = !vertical;
    layoutHeightField.hidden = vertical;
  }

  function setLayoutPanelOpen(open) {
    layoutPanel.hidden = !open;
    layoutToggle.setAttribute("aria-expanded", String(open));
    layoutTools.classList.toggle("is-open", open);
  }

  function setLayoutMode(mode) {
    if (layoutSettings.mode === mode) {
      return;
    }
    layoutSettings.mode = mode;
    syncLayoutSettings();
    rebuildLayout();
  }

  function bindLayoutRange(input, output, key, afterChange) {
    input.addEventListener("input", () => {
      const value = Number(input.value);
      layoutSettings[key] = value;
      output.textContent = String(value);
      afterChange();
    });
  }

  bindLayoutRange(
    layoutColGap,
    layoutColGapValue,
    "colGap",
    applyLayoutSettings,
  );
  bindLayoutRange(
    layoutRowGap,
    layoutRowGapValue,
    "rowGap",
    applyLayoutSettings,
  );
  bindLayoutRange(
    layoutRadius,
    layoutRadiusValue,
    "radius",
    applyLayoutSettings,
  );
  bindLayoutRange(
    layoutHeight,
    layoutHeightValue,
    "height",
    applyLayoutSettings,
  );

  layoutColumns.addEventListener("input", () => {
    layoutSettings.columns = Number(layoutColumns.value);
    layoutColumnsValue.textContent = String(layoutSettings.columns);
    rebuildLayout();
  });

  layoutModeVertical.addEventListener("click", () => {
    setLayoutMode("vertical");
  });
  layoutModeHorizontal.addEventListener("click", () => {
    setLayoutMode("horizontal");
  });

  layoutToggle.addEventListener("click", () => {
    setLayoutPanelOpen(layoutPanel.hidden);
  });
  layoutClose.addEventListener("click", () => {
    setLayoutPanelOpen(false);
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && !layoutPanel.hidden) {
      setLayoutPanelOpen(false);
    }
  });
  document.addEventListener("pointerdown", (event) => {
    if (!layoutPanel.hidden && !layoutTools.contains(event.target)) {
      setLayoutPanelOpen(false);
    }
  });

  // ---- 悬停放大预览 ----
  // 指针在缩略图上完全静止 1.5s 后，把这张图等比放大到「鼠标到屏幕对侧
  // 边缘」与「满屏高」构成的框内，贴住鼠标的那条边正好落在鼠标 x 上。
  // 进入大图模式后换图不再等待；离开卡片立刻收起，0.8s 内落到别的卡片
  // 仍可秒开。整个过程只复用已加载的缩略图，不额外请求原图。
  const hoverPreviewImage = hoverPreview.querySelector("img");
  // 指针在空白处停留多久后，大图模式失效（需要重新等 1.5s）。
  const hoverState = {
    active: false,
    shown: false,
    imageId: "",
    naturalWidth: 1,
    naturalHeight: 1,
    pointerX: 0,
    pointerY: 0,
    delayTimer: 0,
    warmTimer: 0,
  };
  // 按 imageId 缓存 regular 尺寸的大图，来回移动时不重复下载。
  const hoverRegularImages = new Map();

  function hideHoverPreview() {
    if (!hoverState.shown) {
      return;
    }
    hoverState.shown = false;
    hoverState.imageId = "";
    hoverPreview.hidden = true;
  }

  function exitHoverPreview() {
    clearTimeout(hoverState.delayTimer);
    clearTimeout(hoverState.warmTimer);
    hoverState.active = false;
    hideHoverPreview();
  }

  // 把浮层摆到鼠标的对侧：鼠标在左半屏，图贴右侧边缘，左边贴鼠标。
  function positionHoverPreview(x, y) {
    const viewportWidth = window.innerWidth;
    const viewportHeight = window.innerHeight;
    const onLeftHalf = x < viewportWidth / 2;
    // 鼠标到它对侧边缘的距离，就是图可用的最大宽度。
    const boxWidth = onLeftHalf ? viewportWidth - x : x;
    const scale = Math.min(
      boxWidth / hoverState.naturalWidth,
      viewportHeight / hoverState.naturalHeight,
    );
    const width = Math.max(1, hoverState.naturalWidth * scale);
    const height = Math.max(1, hoverState.naturalHeight * scale);
    const left = onLeftHalf ? x : x - width;
    // 贴鼠标那条边的中心对齐鼠标 y，越界时夹回视口内。
    const top = Math.min(
      Math.max(y - height / 2, 0),
      Math.max(0, viewportHeight - height),
    );

    hoverPreview.style.left = `${left}px`;
    hoverPreview.style.top = `${top}px`;
    hoverPreview.style.width = `${width}px`;
    hoverPreview.style.height = `${height}px`;
  }

  // 先用卡片上已经加载好的 small 图顶上，再在后台换成 regular。
  function upgradeHoverPreview(imageId) {
    if (!imageId) {
      return;
    }
    let loader = hoverRegularImages.get(imageId);
    if (!loader) {
      loader = new Image();
      loader.decoding = "async";
      loader.src = imageStreamUrl(imageId, "regular");
      hoverRegularImages.set(imageId, loader);
    }

    const apply = () => {
      if (!hoverState.shown || hoverState.imageId !== imageId) {
        return;
      }
      if (loader.naturalWidth > 0 && loader.naturalHeight > 0) {
        hoverState.naturalWidth = loader.naturalWidth;
        hoverState.naturalHeight = loader.naturalHeight;
      }
      hoverPreviewImage.src = loader.src;
      positionHoverPreview(hoverState.pointerX, hoverState.pointerY);
    };

    if (loader.complete && loader.naturalWidth > 0) {
      apply();
    } else {
      loader.onload = apply;
    }
  }

  function showHoverPreview(image, x, y) {
    hoverState.shown = true;
    hoverState.imageId = image.dataset.imageId || "";
    hoverState.naturalWidth =
      image.naturalWidth || Number(image.dataset.imageWidth) || 1;
    hoverState.naturalHeight =
      image.naturalHeight || Number(image.dataset.imageHeight) || 1;
    hoverState.pointerX = x;
    hoverState.pointerY = y;

    hoverPreviewImage.src = image.currentSrc || image.src;
    hoverPreview.hidden = false;
    positionHoverPreview(x, y);
    upgradeHoverPreview(hoverState.imageId);
  }

  function hoverThumbImageAt(target) {
    if (!target || typeof target.closest !== "function") {
      return null;
    }
    const thumb = target.closest(".thumb");
    if (!thumb || !results.contains(thumb)) {
      return null;
    }
    // 占位卡片没有 img，自然也不该触发预览。
    return thumb.querySelector("img");
  }

  // 指针落到空白处：立刻收起，并开始计算「秒开」余温。
  function hoverPreviewBlank() {
    clearTimeout(hoverState.delayTimer);
    clearTimeout(hoverState.warmTimer);
    hideHoverPreview();
    if (hoverState.active) {
      hoverState.warmTimer = setTimeout(() => {
        hoverState.active = false;
      }, HOVER_PREVIEW_WARM);
    }
  }

  results.addEventListener("pointermove", (event) => {
    if (event.pointerType === "touch") {
      exitHoverPreview();
      return;
    }
    const image = hoverThumbImageAt(event.target);
    if (!image) {
      hoverPreviewBlank();
      return;
    }

    // 必须完全静止：任何一次移动都重新计时。
    clearTimeout(hoverState.delayTimer);
    clearTimeout(hoverState.warmTimer);
    hoverState.pointerX = event.clientX;
    hoverState.pointerY = event.clientY;

    if (hoverState.shown && hoverState.imageId === (image.dataset.imageId || "")) {
      // 同一张图：贴着鼠标连续移动。
      positionHoverPreview(event.clientX, event.clientY);
      return;
    }

    if (hoverState.active) {
      // 已经进入大图模式，换图不等延迟。
      showHoverPreview(image, event.clientX, event.clientY);
      return;
    }

    hoverState.delayTimer = setTimeout(() => {
      if (!document.contains(image)) {
        return;
      }
      hoverState.active = true;
      showHoverPreview(
        image,
        hoverState.pointerX,
        hoverState.pointerY,
      );
    }, HOVER_PREVIEW_DELAY);
  });

  results.addEventListener("pointerleave", hoverPreviewBlank);

  // 点击空白区域退出大图模式；点卡片照常跳转。
  document.addEventListener("pointerdown", (event) => {
    if (!hoverThumbImageAt(event.target)) {
      exitHoverPreview();
    }
  });

  window.addEventListener("scroll", hideHoverPreview, { passive: true });

  window.addEventListener("resize", () => {
    if (hoverState.shown) {
      positionHoverPreview(hoverState.pointerX, hoverState.pointerY);
    }
  });

  syncLayoutSettings();
  applyLayoutSettings();

  window.addEventListener("scroll", maybeLoadMore, { passive: true });
  window.addEventListener("resize", () => {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(relayout, 150);
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
