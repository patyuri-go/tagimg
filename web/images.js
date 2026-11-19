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
    const width = results.clientWidth || window.innerWidth;
    if (width < 480) {
      return 1;
    }
    if (width < 780) {
      return 2;
    }
    return Math.max(2, Math.min(5, Math.floor(width / 300)));
  }

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

  function placeCard(card) {
    const column = shortestColumn();
    if (column) {
      column.appendChild(card);
    }
  }

  function rebuildColumns() {
    const count = desiredColumnCount();
    const fragment = document.createDocumentFragment();
    const next = [];
    for (let i = 0; i < count; i += 1) {
      const column = document.createElement("div");
      column.className = "masonry-column";
      next.push(column);
      fragment.appendChild(column);
    }
    results.replaceChildren(fragment);
    columns = next;
    for (const card of allCards) {
      placeCard(card);
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

  function createArtworkList(item) {
    const wrapper = document.createElement("div");
    wrapper.className = "image-artworks";

    const artworks = Array.isArray(item.artworks) ? item.artworks : [];
    if (artworks.length === 0) {
      const orphan = document.createElement("a");
      orphan.className = "image-orphan";
      orphan.href = "/orphans.html";
      orphan.textContent = "游离图片";
      wrapper.appendChild(orphan);
      return wrapper;
    }

    for (const artwork of artworks) {
      const link = document.createElement("a");
      link.className = "image-artwork-link";
      link.href = `/artwork.html?id=${encodeURIComponent(artwork.id)}`;
      link.textContent = artwork.title || "（无标题）";
      link.title = artwork.publishedAt
        ? `${artwork.title}\n发布 ${formatDate(artwork.publishedAt)}`
        : artwork.title;
      wrapper.appendChild(link);
    }
    return wrapper;
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
    body.appendChild(createArtworkList(item));
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

      if (data.items.length > 0 && columns.length !== desiredColumnCount()) {
        rebuildColumns();
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

  function renderLightbox() {
    const item = lightboxEntries[lightboxIndex];
    if (!item) {
      return;
    }
    const imageId = item.id;
    const token = (lightboxToken += 1);
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

  lightbox.addEventListener("close", () => {
    document.body.classList.remove("lightbox-open");
    lightboxToken += 1;
    lightboxPlaceholderSrc = "";
    lightboxEntries = [];
    lightboxIndex = -1;
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

  function positionHoverPreview(x, y) {
    if (!hoverState.shown || !hoverState.naturalWidth || !hoverState.naturalHeight) {
      return;
    }
    const margin = 16;
    const viewportWidth = window.innerWidth;
    const viewportHeight = window.innerHeight;
    const placeRight = x < viewportWidth / 2;
    const available = placeRight ? viewportWidth - x - margin : x - margin;
    const maxWidth = Math.max(1, Math.min(available, viewportWidth * 0.62));
    const maxHeight = Math.max(1, viewportHeight - margin * 2);
    const scale = Math.min(
      maxWidth / hoverState.naturalWidth,
      maxHeight / hoverState.naturalHeight,
      1,
    );
    const width = Math.max(1, hoverState.naturalWidth * scale);
    const height = Math.max(1, hoverState.naturalHeight * scale);

    let left = placeRight ? x + margin : x - margin - width;
    left = Math.max(margin, Math.min(left, viewportWidth - margin - width));
    let top = y - height / 2;
    top = Math.max(margin, Math.min(top, viewportHeight - margin - height));

    hoverPreview.style.width = `${width}px`;
    hoverPreview.style.height = `${height}px`;
    hoverPreview.style.left = `${left}px`;
    hoverPreview.style.top = `${top}px`;
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
    const width =
      Number(image.dataset.imageWidth) || image.naturalWidth || 0;
    const height =
      Number(image.dataset.imageHeight) || image.naturalHeight || 0;
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

  function hoverPreviewBlank() {
    window.clearTimeout(hoverState.delayTimer);
    hideHoverPreview();
    if (!hoverState.active) {
      return;
    }
    window.clearTimeout(hoverState.warmTimer);
    hoverState.warmTimer = window.setTimeout(() => {
      hoverState.active = false;
    }, HOVER_PREVIEW_WARM);
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

    window.clearTimeout(hoverState.delayTimer);
    hoverState.pointerX = event.clientX;
    hoverState.pointerY = event.clientY;

    if (hoverState.active) {
      window.clearTimeout(hoverState.warmTimer);
      if (hoverState.imageId !== image.dataset.imageId) {
        showHoverPreview(image, event.clientX, event.clientY);
      } else {
        positionHoverPreview(event.clientX, event.clientY);
      }
      return;
    }

    if (hoverState.shown && hoverState.imageId === image.dataset.imageId) {
      positionHoverPreview(event.clientX, event.clientY);
      return;
    }

    hoverState.delayTimer = window.setTimeout(() => {
      if (!image.isConnected || !results.contains(image)) {
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
      if (columns.length !== desiredColumnCount()) {
        rebuildColumns();
      }
    }, 150);
  });

  restoreStateFromUrl();
  renderStaging();
  renderReferenceBar();
  syncSortUi();
  syncStateToUrl();
  search(true);
})();
