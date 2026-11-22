(() => {
  "use strict";

  const PAGE_SIZE = 24;
  const LOAD_MORE_MARGIN = 400;
  const HOVER_PREVIEW_DELAY = 1500;
  const HOVER_PREVIEW_WARM = 800;
  const UUID_PATTERN =
    /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

  const ORDER_VALUES = ["created", "published", "scan", "similarity"];
  const ORDER_ENUMS = {
    created: "CREATED_AT",
    published: "PUBLISHED_AT",
    scan: "SCAN_AT",
    similarity: "SIMILARITY",
  };

  const SEARCH_IMAGES_QUERY = `
    query SearchImages(
      $filter: ImageFilter
      $orderBy: ImageOrderField!
      $limit: Int!
      $offset: Int!
      $sortText: String
      $referenceImageId: ID
    ) {
      images(
        filter: $filter
        orderBy: $orderBy
        limit: $limit
        offset: $offset
        includeTotal: true
        sortText: $sortText
        referenceImageId: $referenceImageId
      ) {
        items {
          id
          width
          height
          frameCount
          createdAt
          scanAt
          publishedAt
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
          artworks {
            id
            title
            publishedAt
          }
        }
        hasMore
        total
      }
    }
  `;

  const tagInput = document.getElementById("tag-input");
  const tagSuggest = document.getElementById("tag-suggest");
  const orderSelect = document.getElementById("image-order");
  const sortSearch = document.getElementById("image-sort-search");
  const sortInput = document.getElementById("image-sort-input");
  const sortClear = document.getElementById("image-sort-clear");
  const referenceBar = document.getElementById("image-reference");
  const referenceThumb = document.getElementById("image-reference-thumb");
  const referenceIdText = document.getElementById("image-reference-id");
  const referenceClear = document.getElementById("image-reference-clear");
  const tagStaging = document.getElementById("tag-staging");
  const includeStageRow = document.getElementById("include-stage-row");
  const excludeStageRow = document.getElementById("exclude-stage-row");
  const includeStage = document.getElementById("include-stage");
  const excludeStage = document.getElementById("exclude-stage");
  const statusLine = document.getElementById("image-status");
  const results = document.getElementById("image-results");
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
  const hoverPreview = document.getElementById("hover-preview");
  const hoverPreviewImage = hoverPreview.querySelector("img");

  const lightbox = document.getElementById("image-lightbox");
  const lightboxClose = document.getElementById("image-lightbox-close");
  const lightboxCaption = document.getElementById("image-lightbox-caption");
  const lightboxStage = document.getElementById("image-lightbox-stage");
  const lightboxImage = document.getElementById("image-lightbox-image");
  const lightboxError = document.getElementById("image-lightbox-error");
  const lightboxPrev = document.getElementById("image-lightbox-prev");
  const lightboxNext = document.getElementById("image-lightbox-next");
  const lightboxArtworks = document.getElementById("image-lightbox-artworks");
  const lightboxTags = document.getElementById("image-lightbox-tags");
  const lightboxMeta = document.getElementById("image-lightbox-meta");
  const lightboxSimilar = document.getElementById("image-lightbox-similar");

  const ZOOM_MIN = 1;
  const ZOOM_MAX = 8;
  const lightboxZoom = {
    scale: 1,
    x: 0,
    y: 0,
    dragging: false,
    pointerId: null,
    startX: 0,
    startY: 0,
    originX: 0,
    originY: 0,
  };

  const includeTags = [];
  const excludeTags = [];

  let order = "created";
  let sortText = "";
  let referenceImageId = "";
  let offset = 0;
  let inFlight = null;
  let hasMore = false;
  let allItems = [];
  let allCards = [];
  let columns = [];
  let resizeTimer = 0;

  // 排版设置只影响卡片排列和外观，不改动后端数据。
  const layoutSettings = {
    mode: "vertical",
    colGap: 18,
    rowGap: 18,
    radius: 14,
    columns: 4,
    height: 240,
  };
  // 默认跟随窗口宽度；拖动列数滑杆后改为手动，刷新页面恢复自动。
  let layoutAuto = true;
  let layoutManualColumns = 4;

  let lightboxEntries = [];
  let lightboxIndex = -1;
  let lightboxToken = 0;
  let lightboxPlaceholderSrc = "";

  const hoverState = {
    active: false,
    shown: false,
    imageId: "",
    naturalWidth: 0,
    naturalHeight: 0,
    pointerX: 0,
    pointerY: 0,
    delayTimer: 0,
    warmTimer: 0,
  };
  // 悬停预览和灯箱各自缓存 regular 大图。
  const hoverRegularImages = new Map();
  const lightboxRegularImages = new Map();

  function setStatus(text, kind) {
    const value = text || "";
    statusLine.textContent = value;
    statusLine.hidden = value === "";
    statusLine.classList.toggle("error", kind === "error");
  }

  function formatDate(value) {
    if (!value) {
      return "";
    }
    return value.replace("T", " ").replace("Z", "").slice(0, 16);
  }

  function imageStreamUrl(imageId, size) {
    const url = `/api/v1/image/${encodeURIComponent(imageId)}`;
    return size ? `${url}?size=${encodeURIComponent(size)}` : url;
  }

  function cachedRegularImage(cache, imageId) {
    let image = cache.get(imageId);
    if (!image) {
      image = new Image();
      image.decoding = "async";
      image.fetchPriority = "low";
      image.src = imageStreamUrl(imageId, "regular");
      cache.set(imageId, image);
    }
    return image;
  }

  function tagTooltip(tag, path) {
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

  function pathMapOf(item) {
    const map = new Map();
    for (const entry of item.tagPaths || []) {
      map.set(entry.id, entry.path);
    }
    return map;
  }

  function artworkTitles(item) {
    const titles = (item.artworks || [])
      .map((artwork) => artwork.title)
      .filter((title) => Boolean(title));
    return titles;
  }

  function hasReferenceImage() {
    return referenceImageId.length > 0;
  }

  function buildSimilarUrl(imageId) {
    const params = new URLSearchParams(window.location.search);
    params.delete("order");
    params.delete("sort");
    params.set("similar", imageId);
    return `${window.location.pathname}?${params.toString()}`;
  }

  function renderReferenceBar() {
    const active = hasReferenceImage();
    referenceBar.hidden = !active;
    if (!active) {
      referenceThumb.hidden = false;
      referenceThumb.removeAttribute("src");
      referenceIdText.textContent = "";
      return;
    }
    referenceThumb.hidden = false;
    const thumbSrc = imageStreamUrl(referenceImageId, "thumb");
    if (referenceThumb.getAttribute("src") !== thumbSrc) {
      referenceThumb.src = thumbSrc;
    }
    referenceIdText.textContent = referenceImageId;
  }

  function isSimilarityActive() {
    return (
      hasReferenceImage() || (order === "similarity" && sortText.length > 0)
    );
  }

  function effectiveOrderBy() {
    if (isSimilarityActive()) {
      return "SIMILARITY";
    }
    if (order === "similarity") {
      return "CREATED_AT";
    }
    return ORDER_ENUMS[order] || "CREATED_AT";
  }

  // ---------- 标签暂存 ----------

  function addStageTag(tag, kind) {
    const target = kind === "exclude" ? excludeTags : includeTags;
    const other = kind === "exclude" ? includeTags : excludeTags;
    const otherIndex = other.findIndex((item) => item.id === tag.id);
    if (otherIndex >= 0) {
      other.splice(otherIndex, 1);
    }
    if (!target.some((item) => item.id === tag.id)) {
      target.push({ id: tag.id, name: tag.name, path: tag.path });
    }
    renderStaging();
    syncStateToUrl();
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
    syncStateToUrl();
    search(true);
  }

  function fillStage(listElement, tags, kind) {
    const fragment = document.createDocumentFragment();
    for (const tag of tags) {
      const chip = document.createElement("button");
      chip.type = "button";
      chip.className = "chip chip-filter";
      chip.textContent = tag.name;
      chip.title = `${tag.path || tag.name}\n点击移除`;
      chip.addEventListener("click", () => removeStageTag(tag.id, kind));
      fragment.appendChild(chip);
    }
    listElement.replaceChildren(fragment);
  }

  function renderStaging() {
    fillStage(includeStage, includeTags, "include");
    fillStage(excludeStage, excludeTags, "exclude");
    includeStageRow.hidden = includeTags.length === 0;
    excludeStageRow.hidden = excludeTags.length === 0;
    tagStaging.hidden =
      includeTags.length === 0 && excludeTags.length === 0;
  }

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

    tagInput.value = `${tag.path}::`;
    tagSuggestWidget.clearChosen();
    tagSuggestWidget.close();
    tagInput.focus();
    tagInput.setSelectionRange(tagInput.value.length, tagInput.value.length);
    addStageTag(tag, kind);
  }

  // ---------- URL ----------

  function appendTagParams(params, tags, idKey, nameKey, pathKey) {
    for (const tag of tags) {
      params.append(idKey, tag.id);
      params.append(nameKey, tag.name);
      if (tag.path) {
        params.append(pathKey, tag.path);
      }
    }
  }

  function restoreTagList(params, idKey, nameKey, pathKey, target) {
    const ids = params.getAll(idKey);
    const names = params.getAll(nameKey);
    const paths = params.getAll(pathKey);
    for (let i = 0; i < ids.length; i += 1) {
      const id = (ids[i] || "").trim().toLowerCase();
      if (!UUID_PATTERN.test(id) || target.some((tag) => tag.id === id)) {
        continue;
      }
      const path = paths[i] || "";
      const name =
        (names[i] || "").trim() ||
        (path ? path.split("::").pop() : "") ||
        "标签";
      target.push({ id, name, path });
    }
  }

  function restoreStateFromUrl() {
    const params = new URLSearchParams(window.location.search);
    restoreTagList(params, "tag", "name", "path", includeTags);
    restoreTagList(
      params,
      "excludeTag",
      "excludeName",
      "excludePath",
      excludeTags,
    );

    // 同一个标签不能既包含又不包含，URL 冲突时以排除为准。
    for (const tag of excludeTags) {
      const index = includeTags.findIndex((item) => item.id === tag.id);
      if (index >= 0) {
        includeTags.splice(index, 1);
      }
    }

    const rawSimilar = (params.get("similar") || "").trim().toLowerCase();
    if (UUID_PATTERN.test(rawSimilar)) {
      referenceImageId = rawSimilar;
      order = "similarity";
      sortText = "";
      return;
    }

    const rawOrder = (params.get("order") || "").trim();
    if (ORDER_VALUES.includes(rawOrder)) {
      order = rawOrder;
    }
    const rawSort = (params.get("sort") || "").trim();
    if (rawSort) {
      sortText = rawSort;
      order = "similarity";
    }
  }

  function syncStateToUrl() {
    const params = new URLSearchParams();
    appendTagParams(params, includeTags, "tag", "name", "path");
    appendTagParams(
      params,
      excludeTags,
      "excludeTag",
      "excludeName",
      "excludePath",
    );
    if (hasReferenceImage()) {
      params.set("similar", referenceImageId);
    } else {
      if (order !== "created") {
        params.set("order", order);
      }
      if (sortText) {
        params.set("sort", sortText);
      }
    }
    const query = params.toString();
    const url = query
      ? `${window.location.pathname}?${query}`
      : window.location.pathname;
    window.history.replaceState(null, "", url);
  }

  // ---------- 请求 ----------

  function currentFilter() {
    const filter = {};
    if (includeTags.length > 0) {
      filter.includeTagIds = includeTags.map((tag) => tag.id);
      filter.includeMode = "ALL";
    }
    if (excludeTags.length > 0) {
      filter.excludeTagIds = excludeTags.map((tag) => tag.id);
    }
    return Object.keys(filter).length > 0 ? filter : null;
  }

  async function requestImages(filter, fromOffset, signal) {
    const response = await fetch("/graphql", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        query: SEARCH_IMAGES_QUERY,
        variables: {
          filter,
          orderBy: effectiveOrderBy(),
          limit: PAGE_SIZE,
          offset: fromOffset,
          sortText:
            !hasReferenceImage() && isSimilarityActive() ? sortText : null,
          referenceImageId: hasReferenceImage() ? referenceImageId : null,
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
      throw new Error(
        payload?.errors?.[0]?.message ||
          `请求失败（HTTP ${response.status}）`,
      );
    }
    if (!payload) {
      throw new Error("服务端返回的不是合法 JSON");
    }
    if (Array.isArray(payload.errors) && payload.errors.length > 0) {
      throw new Error(payload.errors.map((item) => item.message).join("；"));
    }
    const data = payload.data && payload.data.images;
    if (!data) {
      throw new Error("响应缺少 images 数据");
    }
    return data;
  }

  // ---------- 卡片与瀑布流 ----------

  function desiredColumnCount() {
    if (!layoutAuto) {
      return Math.max(1, Math.min(8, Math.round(layoutManualColumns)));
    }
    // 按视口宽度切换列数，保证 780px 起稳定为 4 列。
    const width = window.innerWidth;
    if (width < 480) {
      return 1;
    }
    if (width < 780) {
      return 2;
    }
    return 4;
  }

  // 追加新卡片时只放到当前最短的列，已有卡片不会被重新洗牌。
  function shortestColumn() {
    if (columns.length === 0) {
      return null;
    }
    let target = columns[0];
    let minHeight = target.offsetHeight;
    for (let i = 1; i < columns.length; i += 1) {
      const height = columns[i].offsetHeight;
      if (height < minHeight) {
        minHeight = height;
        target = columns[i];
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
    const column = shortestColumn();
    if (column) {
      column.appendChild(card);
    }
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

  function createThumb(item) {
    const width = Number(item.width);
    const height = Number(item.height);
    const ratioWidth = width > 0 ? width : 3;
    const ratioHeight = height > 0 ? height : 2;

    const thumb = document.createElement("div");
    thumb.className = "thumb";
    thumb.style.aspectRatio = `${ratioWidth} / ${ratioHeight}`;

    const image = document.createElement("img");
    image.alt = artworkTitles(item).join("、") || "图片";
    image.loading = "lazy";
    image.decoding = "async";
    image.dataset.imageId = item.id;
    image.dataset.imageWidth = String(ratioWidth);
    image.dataset.imageHeight = String(ratioHeight);
    image.addEventListener(
      "error",
      () => {
        image.remove();
        thumb.classList.add("placeholder");
        const label = document.createElement("span");
        label.className = "thumb-error-label";
        label.textContent = "图片加载失败";
        thumb.appendChild(label);
      },
      { once: true },
    );
    image.src = imageStreamUrl(item.id, "small");
    thumb.appendChild(image);
    return thumb;
  }

  function createScoreBadge(item) {
    const badge = document.createElement("span");
    badge.className = "score-badge";
    if (typeof item.sortScore === "number" && Number.isFinite(item.sortScore)) {
      badge.classList.add("score-badge-hit");
      badge.textContent = item.sortScore.toFixed(3);
      badge.title = `相似度 ${item.sortScore.toFixed(3)}`;
    } else {
      badge.classList.add("score-badge-empty");
      badge.textContent = "无向量";
      badge.title = "该图片还没有向量，无法计算相似度";
    }
    return badge;
  }

  function artworkLabel(item) {
    const artworks = Array.isArray(item.artworks) ? item.artworks : [];
    if (artworks.length === 0) {
      return "游离图片";
    }
    return artworks
      .map((artwork) => artwork.title || "（无标题）")
      .join("、");
  }

  function createTagList(item) {
    const wrapper = document.createElement("div");
    wrapper.className = "tags";

    const tags = Array.isArray(item.tags) ? item.tags : [];
    if (tags.length === 0) {
      return wrapper;
    }
    const paths = pathMapOf(item);
    const shown = tags.slice(0, 6);
    for (const tag of shown) {
      const chip = document.createElement("span");
      chip.className = "chip";
      chip.textContent = tag.name;
      chip.title = tagTooltip(tag, paths.get(tag.id));
      wrapper.appendChild(chip);
    }
    if (tags.length > shown.length) {
      const more = document.createElement("span");
      more.className = "chip chip-more";
      more.textContent = `+${tags.length - shown.length}`;
      more.title = tags
        .slice(shown.length)
        .map((tag) => tag.name)
        .join("、");
      wrapper.appendChild(more);
    }
    return wrapper;
  }

  function createCard(item, index) {
    const card = document.createElement("article");
    card.className = "card image-card";

    const width = Number(item.width);
    const height = Number(item.height);
    const ratioWidth = width > 0 ? width : 3;
    const ratioHeight = height > 0 ? height : 2;
    card.dataset.aspectRatio = String(ratioWidth / ratioHeight);

    const button = document.createElement("button");
    button.type = "button";
    button.className = "image-thumb-button";
    button.setAttribute(
      "aria-label",
      `查看大图：${artworkTitles(item).join("、") || "图片"}`,
    );

    const thumb = createThumb(item);
    if (isSimilarityActive()) {
      thumb.appendChild(createScoreBadge(item));
    }
    button.appendChild(thumb);
    button.addEventListener("click", () => openLightbox(index));

    const body = document.createElement("div");
    body.className = "card-body";

    const title = document.createElement("h2");
    title.className = "card-title";
    title.textContent = artworkLabel(item);

    const meta = document.createElement("div");
    meta.className = "meta";
    const metaText = document.createElement("span");
    metaText.textContent =
      width > 0 && height > 0 ? `${width}×${height}` : "尺寸未知";
    const frameCount = Number(item.frameCount);
    if (frameCount > 1) {
      metaText.textContent += ` · ${frameCount} 帧`;
    }
    meta.appendChild(metaText);

    body.append(title, meta);
    const tags = createTagList(item);
    if (tags.childElementCount > 0) {
      body.appendChild(tags);
    }

    card.appendChild(button);
    card.appendChild(body);
    return card;
  }

  // ---------- 搜索与分页 ----------

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
    if (reset) {
      offset = 0;
      hasMore = false;
      allItems = [];
      allCards = [];
      columns = [];
      results.replaceChildren();
      if (lightbox.open) {
        lightbox.close();
      }
    }

    if (inFlight) {
      inFlight.abort();
    }
    const controller = new AbortController();
    inFlight = controller;
    setStatus(
      reset
        ? hasReferenceImage()
          ? "相似图查询中…"
          : "搜索中…"
        : "加载中…",
    );

    try {
      const data = await requestImages(currentFilter(), offset, controller.signal);
      if (inFlight !== controller) {
        return;
      }

      if (data.items.length > 0) {
        relayout();
      }

      // 图片尺寸已经随结果返回，可以直接同步插入卡片，保持后端顺序。
      const baseIndex = allItems.length;
      for (let i = 0; i < data.items.length; i += 1) {
        const item = data.items[i];
        allItems.push(item);
        const card = createCard(item, baseIndex + i);
        allCards.push(card);
        placeCard(card);
      }

      offset += data.items.length;
      hasMore = Boolean(data.hasMore) && data.items.length > 0;

      if (offset === 0) {
        if (hasReferenceImage()) {
          setStatus("没有找到相似图片");
        } else {
          setStatus(currentFilter() ? "没有符合条件的图片" : "还没有图片");
        }
      } else if (reset && typeof data.total === "number") {
        setStatus(
          hasReferenceImage()
            ? `相似图：共 ${data.total} 张图片`
            : `共 ${data.total} 张图片`,
        );
      } else {
        setStatus(
          hasReferenceImage()
            ? `相似图：已加载 ${offset} 张图片`
            : `已加载 ${offset} 张图片`,
        );
      }
      window.setTimeout(maybeLoadMore, 0);
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

  // ---------- 灯箱 ----------

  function appendMeta(term, value) {
    const dt = document.createElement("dt");
    dt.textContent = term;
    const dd = document.createElement("dd");
    dd.textContent = value;
    lightboxMeta.appendChild(dt);
    lightboxMeta.appendChild(dd);
  }

  function renderLightboxSidebar(item) {
    lightboxSimilar.href = buildSimilarUrl(item.id);
    lightboxArtworks.replaceChildren();
    const artworks = Array.isArray(item.artworks) ? item.artworks : [];
    if (artworks.length === 0) {
      const link = document.createElement("a");
      link.className = "image-artwork-link";
      link.href = "/orphans.html";
      link.textContent = "游离图片";
      lightboxArtworks.appendChild(link);
    } else {
      for (const artwork of artworks) {
        const row = document.createElement("div");
        row.className = "image-lightbox-artwork";

        const link = document.createElement("a");
        link.className = "image-artwork-link";
        link.href = `/artwork.html?id=${encodeURIComponent(artwork.id)}`;
        link.textContent = artwork.title || "（无标题）";

        const meta = document.createElement("span");
        meta.className = "image-artwork-date";
        meta.textContent = artwork.publishedAt
          ? `发布 ${formatDate(artwork.publishedAt)}`
          : "未设置发布时间";

        row.appendChild(link);
        row.appendChild(meta);
        lightboxArtworks.appendChild(row);
      }
    }

    lightboxTags.replaceChildren();
    const tags = Array.isArray(item.tags) ? item.tags : [];
    if (tags.length === 0) {
      const empty = document.createElement("span");
      empty.className = "image-tag-empty";
      empty.textContent = "无标签";
      lightboxTags.appendChild(empty);
    } else {
      const paths = pathMapOf(item);
      for (const tag of tags) {
        const chip = document.createElement("span");
        chip.className = "chip";
        chip.textContent = tag.name;
        chip.title = tagTooltip(tag, paths.get(tag.id));
        lightboxTags.appendChild(chip);
      }
    }

    lightboxMeta.replaceChildren();
    const width = Number(item.width);
    const height = Number(item.height);
    appendMeta(
      "尺寸",
      width > 0 && height > 0 ? `${width} × ${height}` : "未知",
    );
    if (item.frameCount !== null && item.frameCount !== undefined) {
      appendMeta("帧数", String(item.frameCount));
    }
    appendMeta("创建时间", formatDate(item.createdAt) || "未知");
    appendMeta("扫描时间", item.scanAt ? formatDate(item.scanAt) : "从未扫描");
    appendMeta(
      "发布时间",
      item.publishedAt ? formatDate(item.publishedAt) : "未设置",
    );
    appendMeta("图片 ID", item.id);
  }

  function lightboxViewportSize() {
    const style = window.getComputedStyle(lightboxStage);
    const paddingX =
      (parseFloat(style.paddingLeft) || 0) +
      (parseFloat(style.paddingRight) || 0);
    const paddingY =
      (parseFloat(style.paddingTop) || 0) +
      (parseFloat(style.paddingBottom) || 0);
    return {
      width: Math.max(0, lightboxStage.clientWidth - paddingX),
      height: Math.max(0, lightboxStage.clientHeight - paddingY),
    };
  }

  function clampLightboxZoom() {
    const viewport = lightboxViewportSize();
    const imageWidth = lightboxImage.offsetWidth * lightboxZoom.scale;
    const imageHeight = lightboxImage.offsetHeight * lightboxZoom.scale;
    const maxX = Math.max(0, (imageWidth - viewport.width) / 2);
    const maxY = Math.max(0, (imageHeight - viewport.height) / 2);
    lightboxZoom.x = Math.min(maxX, Math.max(-maxX, lightboxZoom.x));
    lightboxZoom.y = Math.min(maxY, Math.max(-maxY, lightboxZoom.y));
  }

  function applyLightboxZoom() {
    lightboxImage.style.transform =
      `translate(${lightboxZoom.x}px, ${lightboxZoom.y}px) ` +
      `scale(${lightboxZoom.scale})`;
    lightboxStage.classList.toggle(
      "is-zoomed",
      lightboxZoom.scale > ZOOM_MIN + 0.001,
    );
  }

  function resetLightboxZoom() {
    lightboxZoom.scale = ZOOM_MIN;
    lightboxZoom.x = 0;
    lightboxZoom.y = 0;
    lightboxZoom.dragging = false;
    lightboxZoom.pointerId = null;
    lightboxImage.style.transform = "";
    lightboxStage.classList.remove("is-zoomed", "is-dragging");
  }

  function zoomLightboxAt(clientX, clientY, factor) {
    if (lightboxImage.hidden || lightboxImage.offsetWidth === 0) {
      return;
    }
    const nextScale = Math.min(
      ZOOM_MAX,
      Math.max(ZOOM_MIN, lightboxZoom.scale * factor),
    );
    if (Math.abs(nextScale - lightboxZoom.scale) < 0.0001) {
      return;
    }
    const rect = lightboxStage.getBoundingClientRect();
    const offsetX = clientX - (rect.left + rect.width / 2);
    const offsetY = clientY - (rect.top + rect.height / 2);
    const ratio = nextScale / lightboxZoom.scale;
    lightboxZoom.x = offsetX - (offsetX - lightboxZoom.x) * ratio;
    lightboxZoom.y = offsetY - (offsetY - lightboxZoom.y) * ratio;
    lightboxZoom.scale = nextScale;
    if (nextScale <= ZOOM_MIN + 0.001) {
      lightboxZoom.x = 0;
      lightboxZoom.y = 0;
    }
    clampLightboxZoom();
    applyLightboxZoom();
  }

  function renderLightbox() {
    const item = lightboxEntries[lightboxIndex];
    if (!item) {
      return;
    }
    const imageId = item.id;
    const token = (lightboxToken += 1);
    resetLightboxZoom();
    const total = lightboxEntries.length;
    lightboxCaption.textContent = `第 ${lightboxIndex + 1} / ${total} 张`;
    lightboxPrev.hidden = total <= 1;
    lightboxNext.hidden = total <= 1;

    lightboxError.hidden = true;
    lightboxImage.hidden = false;
    lightboxImage.alt =
      artworkTitles(item).join("、") || `图片 ${lightboxIndex + 1}`;
    lightboxImage.dataset.imageId = imageId;

    // 先用列表里已经加载过的缩略图顶上，regular 到达后再替换。
    const placeholderSrc = imageStreamUrl(imageId, "small");
    lightboxPlaceholderSrc = placeholderSrc;
    lightboxImage.src = placeholderSrc;

    const loader = cachedRegularImage(lightboxRegularImages, imageId);
    const applyRegular = () => {
      if (
        token !== lightboxToken ||
        lightboxImage.dataset.imageId !== imageId ||
        loader.naturalWidth <= 0
      ) {
        return;
      }
      lightboxImage.src = loader.currentSrc || loader.src;
    };
    if (loader.complete) {
      applyRegular();
    } else {
      loader.addEventListener("load", applyRegular, { once: true });
    }

    renderLightboxSidebar(item);
  }

  function openLightbox(index) {
    if (index < 0 || index >= allItems.length) {
      return;
    }
    exitHoverPreview();
    lightboxEntries = allItems.slice();
    lightboxIndex = index;
    renderLightbox();
    if (!lightbox.open) {
      lightbox.showModal();
      document.body.classList.add("lightbox-open");
    }
    lightboxClose.focus({ preventScroll: true });
  }

  function moveLightbox(step) {
    const total = lightboxEntries.length;
    if (total <= 1) {
      return;
    }
    lightboxIndex = (lightboxIndex + step + total) % total;
    renderLightbox();
  }

  lightboxImage.addEventListener("load", () => {
    lightboxError.hidden = true;
    lightboxImage.hidden = false;
    if (lightboxZoom.scale > ZOOM_MIN + 0.001) {
      clampLightboxZoom();
      applyLightboxZoom();
    }
  });

  lightboxImage.addEventListener("error", () => {
    const src = lightboxImage.getAttribute("src");
    if (!src) {
      return;
    }
    // regular 失败时退回已经缓存的缩略图，只有缩略图也失败才报错。
    if (lightboxPlaceholderSrc && src !== lightboxPlaceholderSrc) {
      lightboxImage.src = lightboxPlaceholderSrc;
      return;
    }
    lightboxImage.hidden = true;
    lightboxError.hidden = false;
  });

  lightboxClose.addEventListener("click", () => lightbox.close());
  lightboxPrev.addEventListener("click", () => moveLightbox(-1));
  lightboxNext.addEventListener("click", () => moveLightbox(1));

  lightbox.addEventListener("click", (event) => {
    if (event.target === lightbox) {
      lightbox.close();
    }
  });

  lightboxStage.addEventListener("click", (event) => {
    if (event.target === lightboxStage) {
      lightbox.close();
    }
  });

  lightboxStage.addEventListener(
    "wheel",
    (event) => {
      if (!lightbox.open || event.deltaY === 0) {
        return;
      }
      event.preventDefault();
      const factor = Math.exp(-event.deltaY * 0.0015);
      zoomLightboxAt(event.clientX, event.clientY, factor);
    },
    { passive: false },
  );

  lightboxImage.addEventListener("dragstart", (event) => {
    event.preventDefault();
  });

  lightboxImage.addEventListener("pointerdown", (event) => {
    if (
      !lightbox.open ||
      event.button !== 0 ||
      lightboxZoom.scale <= ZOOM_MIN + 0.001
    ) {
      return;
    }
    event.preventDefault();
    lightboxZoom.dragging = true;
    lightboxZoom.pointerId = event.pointerId;
    lightboxZoom.startX = event.clientX;
    lightboxZoom.startY = event.clientY;
    lightboxZoom.originX = lightboxZoom.x;
    lightboxZoom.originY = lightboxZoom.y;
    lightboxStage.classList.add("is-dragging");
    try {
      lightboxImage.setPointerCapture(event.pointerId);
    } catch {
      // 某些浏览器在 pointerdown 里不能立即捕获，忽略即可。
    }
  });

  lightboxImage.addEventListener("pointermove", (event) => {
    if (
      !lightboxZoom.dragging ||
      event.pointerId !== lightboxZoom.pointerId
    ) {
      return;
    }
    event.preventDefault();
    lightboxZoom.x =
      lightboxZoom.originX + (event.clientX - lightboxZoom.startX);
    lightboxZoom.y =
      lightboxZoom.originY + (event.clientY - lightboxZoom.startY);
    clampLightboxZoom();
    applyLightboxZoom();
  });

  function endLightboxDrag(event) {
    if (
      !lightboxZoom.dragging ||
      (event && event.pointerId !== lightboxZoom.pointerId)
    ) {
      return;
    }
    lightboxZoom.dragging = false;
    lightboxZoom.pointerId = null;
    lightboxStage.classList.remove("is-dragging");
  }

  lightboxImage.addEventListener("pointerup", endLightboxDrag);
  lightboxImage.addEventListener("pointercancel", endLightboxDrag);
  lightboxImage.addEventListener("lostpointercapture", endLightboxDrag);

  lightbox.addEventListener("close", () => {
    document.body.classList.remove("lightbox-open");
    lightboxToken += 1;
    lightboxPlaceholderSrc = "";
    lightboxEntries = [];
    lightboxIndex = -1;
    resetLightboxZoom();
    lightboxCaption.textContent = "";
    lightboxImage.removeAttribute("src");
    lightboxImage.removeAttribute("data-image-id");
    lightboxImage.alt = "";
    lightboxImage.hidden = false;
    lightboxError.hidden = true;
    lightboxArtworks.replaceChildren();
    lightboxTags.replaceChildren();
    lightboxMeta.replaceChildren();
  });

  document.addEventListener("keydown", (event) => {
    if (!lightbox.open) {
      return;
    }
    if (event.key === "ArrowLeft") {
      event.preventDefault();
      moveLightbox(-1);
    } else if (event.key === "ArrowRight") {
      event.preventDefault();
      moveLightbox(1);
    }
  });

  // ---------- 悬停大图预览 ----------

  function hideHoverPreview() {
    if (!hoverState.shown) {
      return;
    }
    hoverState.shown = false;
    hoverState.imageId = "";
    hoverPreview.hidden = true;
    hoverPreviewImage.removeAttribute("src");
  }

  function exitHoverPreview() {
    window.clearTimeout(hoverState.delayTimer);
    window.clearTimeout(hoverState.warmTimer);
    hoverState.active = false;
    hideHoverPreview();
  }

  // 把浮层摆到鼠标的对侧：鼠标在左半屏，图贴右侧边缘，左边贴鼠标。
  function positionHoverPreview(x, y) {
    if (!hoverState.shown || !hoverState.naturalWidth || !hoverState.naturalHeight) {
      return;
    }
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

  function applyUpgradedHoverImage(imageId, image) {
    if (!hoverState.shown || hoverState.imageId !== imageId) {
      return;
    }
    if (image.naturalWidth > 0 && image.naturalHeight > 0) {
      hoverState.naturalWidth = image.naturalWidth;
      hoverState.naturalHeight = image.naturalHeight;
      positionHoverPreview(hoverState.pointerX, hoverState.pointerY);
    }
    // regular 预加载完成后再替换，缩略图在请求期间保持可见。
    hoverPreviewImage.src = image.currentSrc || image.src;
  }

  function scheduleHoverUpgrade(imageId) {
    let scheduled = false;
    const run = () => {
      if (scheduled) {
        return;
      }
      scheduled = true;
      if (!hoverState.shown || hoverState.imageId !== imageId) {
        return;
      }
      upgradeHoverPreview(imageId);
    };
    const afterPaint = () => {
      // 先让缩略图完成一次绘制，再在后台请求 regular，避免请求抢占首帧。
      if (typeof window.requestAnimationFrame === "function") {
        window.requestAnimationFrame(() => {
          window.requestAnimationFrame(run);
        });
      } else {
        window.setTimeout(run, 0);
      }
    };
    // decode() 成功表示缩略图已经可以立即绘制；失败时也照常尝试升级。
    if (typeof hoverPreviewImage.decode === "function") {
      hoverPreviewImage.decode().then(afterPaint, afterPaint);
    } else {
      afterPaint();
    }
  }

  function upgradeHoverPreview(imageId) {
    const loader = cachedRegularImage(hoverRegularImages, imageId);
    const apply = () => {
      if (!hoverState.shown || hoverState.imageId !== imageId) {
        return;
      }
      if (loader.naturalWidth > 0 && loader.naturalHeight > 0) {
        applyUpgradedHoverImage(imageId, loader);
      }
    };
    if (loader.complete) {
      apply();
    } else {
      loader.addEventListener("load", apply, { once: true });
    }
  }

  function showHoverPreview(image, x, y) {
    const imageId = image.dataset.imageId;
    if (!imageId) {
      return;
    }
    // 与作品页一致：先用缩略图实际尺寸，regular 到达后再升级。
    const width =
      image.naturalWidth || Number(image.dataset.imageWidth) || 0;
    const height =
      image.naturalHeight || Number(image.dataset.imageHeight) || 0;
    if (!width || !height) {
      return;
    }

    hoverState.imageId = imageId;
    hoverState.naturalWidth = width;
    hoverState.naturalHeight = height;
    hoverState.pointerX = x;
    hoverState.pointerY = y;
    hoverState.shown = true;
    hoverPreviewImage.src = image.currentSrc || image.src;
    hoverPreview.hidden = false;
    positionHoverPreview(x, y);
    scheduleHoverUpgrade(imageId);
  }

  function hoverThumbImageAt(target) {
    if (!(target instanceof Element)) {
      return null;
    }
    const thumb = target.closest(".thumb");
    if (!thumb || !results.contains(thumb)) {
      return null;
    }
    const image = thumb.querySelector("img");
    return image && image.dataset.imageId ? image : null;
  }

  // 指针落到空白处：立刻收起，并开始计算「秒开」余温。
  function hoverPreviewBlank() {
    window.clearTimeout(hoverState.delayTimer);
    window.clearTimeout(hoverState.warmTimer);
    hideHoverPreview();
    if (hoverState.active) {
      hoverState.warmTimer = window.setTimeout(() => {
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
    window.clearTimeout(hoverState.delayTimer);
    window.clearTimeout(hoverState.warmTimer);
    hoverState.pointerX = event.clientX;
    hoverState.pointerY = event.clientY;

    if (
      hoverState.shown &&
      hoverState.imageId === (image.dataset.imageId || "")
    ) {
      // 同一张图：贴着鼠标连续移动。
      positionHoverPreview(event.clientX, event.clientY);
      return;
    }

    if (hoverState.active) {
      // 已经进入大图模式，换图不等延迟。
      showHoverPreview(image, event.clientX, event.clientY);
      return;
    }

    hoverState.delayTimer = window.setTimeout(() => {
      if (!document.contains(image)) {
        return;
      }
      hoverState.active = true;
      showHoverPreview(image, hoverState.pointerX, hoverState.pointerY);
    }, HOVER_PREVIEW_DELAY);
  });

  results.addEventListener("pointerleave", hoverPreviewBlank);

  document.addEventListener("pointerdown", (event) => {
    if (!hoverThumbImageAt(event.target)) {
      exitHoverPreview();
    }
  });

  // ---------- 排序控件 ----------

  function syncSortUi() {
    const hasReference = hasReferenceImage();
    const similarityOption = orderSelect.querySelector(
      'option[value="similarity"]',
    );
    if (similarityOption) {
      similarityOption.disabled = hasReference;
    }
    orderSelect.value = hasReference ? "similarity" : order;
    const show = !hasReference && order === "similarity";
    sortSearch.hidden = !show;
    sortInput.value = hasReference ? "" : sortText;
    sortClear.hidden = sortInput.value.length === 0;
    sortSearch.classList.toggle("is-active", show && sortText.length > 0);
    orderSelect.title = hasReference
      ? "当前按参考图相似度排序；选择其他排序会退出相似图查询"
      : "";
  }

  function leaveReferenceMode(nextOrder) {
    referenceImageId = "";
    order = ORDER_VALUES.includes(nextOrder) ? nextOrder : "created";
    sortText = "";
    renderReferenceBar();
    syncSortUi();
    syncStateToUrl();
    search(true);
  }

  function applySortText(value) {
    if (hasReferenceImage()) {
      return;
    }
    const next = String(value || "").trim();
    sortInput.value = next;
    if (next === sortText && order === "similarity") {
      syncSortUi();
      return;
    }

    sortText = next;
    if (sortText) {
      order = "similarity";
    } else if (order === "similarity") {
      order = "created";
    }
    syncSortUi();
    syncStateToUrl();
    search(true);
  }

  orderSelect.addEventListener("change", () => {
    const next = ORDER_VALUES.includes(orderSelect.value)
      ? orderSelect.value
      : "created";
    if (hasReferenceImage()) {
      if (next === "similarity") {
        syncSortUi();
        return;
      }
      leaveReferenceMode(next);
      return;
    }
    order = next;
    if (order !== "similarity") {
      sortText = "";
    }
    syncSortUi();
    syncStateToUrl();
    search(true);
    if (order === "similarity") {
      sortInput.focus();
    }
  });

  referenceClear.addEventListener("click", () => {
    if (!hasReferenceImage()) {
      return;
    }
    leaveReferenceMode("created");
  });

  referenceThumb.addEventListener("load", () => {
    referenceThumb.hidden = false;
  });

  referenceThumb.addEventListener("error", () => {
    referenceThumb.hidden = true;
  });

  sortInput.addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      event.preventDefault();
      applySortText(sortInput.value);
    }
  });

  sortInput.addEventListener("input", () => {
    if (sortInput.value === "") {
      applySortText("");
      return;
    }
    sortClear.hidden = false;
  });

  sortClear.addEventListener("click", () => {
    applySortText("");
    sortInput.focus();
  });

  // ---------- 排版面板 ----------

  function syncLayoutSettings() {
    const vertical = layoutSettings.mode === "vertical";
    layoutColGap.value = String(layoutSettings.colGap);
    layoutColGapValue.textContent = String(layoutSettings.colGap);
    layoutRowGap.value = String(layoutSettings.rowGap);
    layoutRowGapValue.textContent = String(layoutSettings.rowGap);
    layoutRadius.value = String(layoutSettings.radius);
    layoutRadiusValue.textContent = String(layoutSettings.radius);
    const columns = desiredColumnCount();
    layoutColumns.value = String(columns);
    layoutColumnsValue.textContent = String(columns);
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
    layoutAuto = false;
    layoutManualColumns = Number(layoutColumns.value);
    layoutSettings.columns = layoutManualColumns;
    layoutColumnsValue.textContent = String(layoutManualColumns);
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

  // ---------- 事件绑定与启动 ----------

  const tagSuggestWidget = TagSearch.create({
    input: tagInput,
    list: tagSuggest,
    idPrefix: "image-tag",
    onEnter: () => addTagFromInput("include"),
    onShiftEnter: () => addTagFromInput("exclude"),
  });

  window.addEventListener(
    "scroll",
    () => {
      hideHoverPreview();
      maybeLoadMore();
    },
    { passive: true },
  );

  window.addEventListener("resize", () => {
    if (hoverState.active) {
      positionHoverPreview(hoverState.pointerX, hoverState.pointerY);
    }
    window.clearTimeout(resizeTimer);
    resizeTimer = window.setTimeout(() => {
      if (layoutAuto) {
        syncLayoutSettings();
      }
      relayout();
    }, 150);
  });

  syncLayoutSettings();
  restoreStateFromUrl();
  renderStaging();
  renderReferenceBar();
  syncSortUi();
  syncStateToUrl();
  search(true);
})();
