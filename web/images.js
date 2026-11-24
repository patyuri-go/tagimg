import { ImageLightbox } from "/lightbox.js";
import { createTagSearch } from "/tag-search.js";

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

  const TAG_CHILDREN_QUERY = `
    query TagChildren($parentId: ID) {
      tagChildren(parentId: $parentId) {
        id
        parentId
        name
        aliases
        path
        hasChildren
      }
    }
  `;

  const TAG_TREE_SEARCH_QUERY = `
    query TagTreeSearch($query: String!) {
      tagTreeSearch(query: $query) {
        node {
          id
          parentId
          name
          aliases
          path
          hasChildren
        }
        ancestors {
          id
          parentId
          name
          aliases
          path
          hasChildren
        }
        matchedName
        matchedAlias
      }
    }
  `;

  const CREATE_TAG_MUTATION = `
    mutation CreateTag($input: CreateTagInput!) {
      createTag(input: $input) {
        id
        parentId
        name
        aliases
      }
    }
  `;

  const UPDATE_TAG_MUTATION = `
    mutation UpdateTag($id: ID!, $input: UpdateTagInput!) {
      updateTag(id: $id, input: $input) {
        id
        parentId
        name
        aliases
      }
    }
  `;

  const UPDATE_IMAGES_MUTATION = `
    mutation UpdateImages($input: UpdateImagesInput!) {
      updateImages(input: $input)
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
  const includeModeToggle = document.getElementById("include-mode-toggle");
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

  const tagDrawer = document.getElementById("tag-drawer");
  const tagDrawerHandle = document.getElementById("tag-drawer-handle");
  const tagDrawerPanel = document.getElementById("tag-drawer-panel");
  const tagDrawerClose = document.getElementById("tag-drawer-close");
  const tagTreeSearch = document.getElementById("tag-tree-search");
  const tagTreeSearchClear = document.getElementById("tag-tree-search-clear");
  const tagEditButton = document.getElementById("tag-edit-button");
  const tagTreeStatus = document.getElementById("tag-tree-status");
  const tagTree = document.getElementById("tag-tree");
  const tagEditor = document.getElementById("tag-editor");
  const tagEditorForm = document.getElementById("tag-editor-form");
  const tagEditorTitle = document.getElementById("tag-editor-title");
  const tagEditorClose = document.getElementById("tag-editor-close");
  const tagEditorModeEdit = document.getElementById("tag-editor-mode-edit");
  const tagEditorModeAdd = document.getElementById("tag-editor-mode-add");
  const tagEditorName = document.getElementById("tag-editor-name");
  const tagEditorAliases = document.getElementById("tag-editor-aliases");
  const tagEditorAliasInput = document.getElementById("tag-editor-alias-input");
  const tagEditorParentField = document.getElementById("tag-editor-parent-field");
  const tagEditorParentDisplay = document.getElementById(
    "tag-editor-parent-display",
  );
  const tagEditorParentClear = document.getElementById("tag-editor-parent-clear");
  const tagEditorParentSearch = document.getElementById(
    "tag-editor-parent-search",
  );
  const tagEditorParentInput = document.getElementById("tag-editor-parent-input");
  const tagEditorParentSuggest = document.getElementById(
    "tag-editor-parent-suggest",
  );
  const tagEditorError = document.getElementById("tag-editor-error");
  const tagEditorCancel = document.getElementById("tag-editor-cancel");
  const tagEditorSave = document.getElementById("tag-editor-save");

  const selectionCard = document.getElementById("selection-card");
  const selectionCount = document.getElementById("selection-count");
  const selectionClose = document.getElementById("selection-close");
  const selectionCommonTags = document.getElementById("selection-common-tags");
  const selectionTagInput = document.getElementById("selection-tag-input");
  const selectionTagSuggest = document.getElementById("selection-tag-suggest");
  const selectionLockHint = document.getElementById("selection-lock-hint");
  const selectionError = document.getElementById("selection-error");
  const selectionCancel = document.getElementById("selection-cancel");
  const selectionSave = document.getElementById("selection-save");

  const workbenchHandle = document.getElementById("workbench-handle");
  const workbenchPanel = document.getElementById("workbench-panel");
  const workbenchClose = document.getElementById("workbench-close");
  const workbenchStatus = document.getElementById("workbench-status");
  const workbenchApply = document.getElementById("workbench-apply");
  const workbenchDotSize = document.getElementById("workbench-dot-size");
  const workbenchDotSizeValue = document.getElementById(
    "workbench-dot-size-value",
  );

  const TAG_DRAWER_WIDTH = 320;
  const WORKBENCH_WIDTH = 360;
  const TAG_TREE_SEARCH_DELAY = 200;

  let tagNodeCache = new Map();
  let tagTreeRootIds = [];
  let tagTreeRootsLoaded = false;
  let tagTreeRootsLoading = false;
  let tagTreeRootsError = false;
  let selectedTagId = "";
  let tagTreeSearchText = "";
  let tagTreeSearchTimer = 0;
  let tagTreeSearchController = null;
  let tagTreeSearchToken = 0;
  let tagTreeSearchActive = false;
  let tagTreeSearchRoots = [];
  let tagTreeSearchLoading = false;
  let tagTreeSearchError = false;
  let tagTreeSearchExpanded = new Map();
  let tagDrawerOpen = false;
  let tagEditorOpen = false;
  let tagEditorMode = "edit";
  let tagEditorTagId = "";
  let tagEditorAliasesValue = [];
  let tagEditorParentId = null;
  let tagEditorParentNode = null;
  let tagEditorOriginalParentId = null;
  let tagEditorSaving = false;
  let tagEditorParentSuggestWidget = null;

  const selectedImageItems = new Map();
  let selectionPendingAdditions = [];
  const selectionPendingRemovals = new Set();
  let selectionSaving = false;
  let selectionTagSuggestWidget = null;
  let suppressSelectionClick = false;
  let suppressSelectionClickTimer = 0;
  const selectionDrag = {
    active: false,
    pointerId: null,
    startX: 0,
    startY: 0,
    lastX: 0,
    lastY: 0,
    moved: false,
    scrollSpeed: 0,
    scrollFrame: 0,
  };

  const WORKBENCH_CARDS = [
    { index: 1, dotClass: "workbench-dot-blue" },
    { index: 2, dotClass: "workbench-dot-amber" },
    { index: 3, dotClass: "workbench-dot-purple" },
    { index: 4, dotClass: "workbench-dot-cyan" },
  ];
  const workbench = {
    open: false,
    applying: false,
    closing: false,
    cards: WORKBENCH_CARDS.map((meta) => ({
      ...meta,
      images: new Set(),
      additions: [],
      removals: [],
      widget: null,
      searchOpen: false,
      elements: null,
    })),
  };
  const workbenchDrag = {
    active: false,
    pointerId: null,
    card: null,
    imageId: "",
    startX: 0,
    startY: 0,
    lastX: 0,
    lastY: 0,
    moved: false,
    scrollSpeed: 0,
    scrollFrame: 0,
  };

  const includeTags = [];
  const excludeTags = [];
  let includeMode = "ALL";

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
  // 悬停预览缓存 regular 大图。
  const hoverRegularImages = new Map();

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
    clearSelection();
    search(true);
  }

  function removeStageTag(id, kind) {
    const list = kind === "exclude" ? excludeTags : includeTags;
    const index = list.findIndex((item) => item.id === id);
    if (index < 0) {
      return;
    }
    list.splice(index, 1);
    if (kind === "include" && includeTags.length === 0) {
      includeMode = "ALL";
    }
    renderStaging();
    syncStateToUrl();
    clearSelection();
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
    if (includeTags.length === 0) {
      includeMode = "ALL";
    }
    fillStage(includeStage, includeTags, "include");
    fillStage(excludeStage, excludeTags, "exclude");
    includeStageRow.hidden = includeTags.length === 0;
    excludeStageRow.hidden = excludeTags.length === 0;
    tagStaging.hidden =
      includeTags.length === 0 && excludeTags.length === 0;
    includeModeToggle.hidden = includeTags.length === 0;
    includeModeToggle.textContent = includeMode;
    includeModeToggle.title =
      includeMode === "ALL"
        ? "当前 ALL，点击切换为 ANY"
        : "当前 ANY，点击切换为 ALL";
  }

  includeModeToggle.addEventListener("click", () => {
    includeMode = includeMode === "ALL" ? "ANY" : "ALL";
    renderStaging();
    syncStateToUrl();
    search(true);
  });

  async function requestGraphQl(query, variables, signal) {
    const response = await fetch("/graphql", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ query, variables }),
      signal,
    });
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    const payload = await response.json();
    if (Array.isArray(payload.errors) && payload.errors.length > 0) {
      throw new Error(payload.errors[0].message || "请求失败");
    }
    return payload.data || {};
  }

  // ---------- 标签树抽屉 ----------

  function rememberTagNode(source) {
    let node = tagNodeCache.get(source.id);
    if (!node) {
      node = {
        id: source.id,
        parentId: null,
        name: "",
        aliases: [],
        path: "",
        hasChildren: false,
        children: [],
        childrenLoaded: false,
        loading: false,
        error: false,
        expanded: false,
      };
      tagNodeCache.set(node.id, node);
    }
    node.parentId = source.parentId || null;
    node.name = source.name || "";
    node.aliases = Array.isArray(source.aliases) ? source.aliases : [];
    node.path = source.path || node.name;
    node.hasChildren = Boolean(source.hasChildren);
    return node;
  }

  function tagNodeFromLoose(tag) {
    return rememberTagNode({
      id: tag.id,
      parentId: tag.parentId || null,
      name: tag.name,
      aliases: tag.aliases || [],
      path: tag.path,
      hasChildren: Boolean(tag.hasChildren),
    });
  }

  function resetTagTree() {
    tagNodeCache = new Map();
    tagTreeRootIds = [];
    tagTreeRootsLoaded = false;
    tagTreeRootsLoading = false;
    tagTreeRootsError = false;
    selectedTagId = "";
    tagTreeSearchExpanded = new Map();
  }

  async function loadTagRoots() {
    if (tagTreeRootsLoading) {
      return;
    }
    tagTreeRootsLoading = true;
    tagTreeRootsError = false;
    renderTagTree();
    try {
      const data = await requestGraphQl(TAG_CHILDREN_QUERY, { parentId: null });
      const roots = (data.tagChildren || []).map(rememberTagNode);
      tagTreeRootIds = roots.map((node) => node.id);
      tagTreeRootsLoaded = true;
      renderTagTree();
      await Promise.all(
        roots
          .filter((node) => node.hasChildren)
          .map((node) => loadTagChildren(node.id, true)),
      );
    } catch {
      tagTreeRootsError = true;
    } finally {
      tagTreeRootsLoading = false;
      renderTagTree();
    }
  }

  async function loadTagChildren(id, expand) {
    const node = tagNodeCache.get(id);
    if (!node || node.loading) {
      return;
    }
    if (node.childrenLoaded) {
      if (expand) {
        node.expanded = true;
        renderTagTree();
      }
      return;
    }

    node.loading = true;
    node.error = false;
    if (expand) {
      node.expanded = true;
    }
    renderTagTree();
    try {
      const data = await requestGraphQl(TAG_CHILDREN_QUERY, { parentId: id });
      const children = (data.tagChildren || []).map(rememberTagNode);
      for (const child of children) {
        child.parentId = id;
      }
      node.children = children.map((child) => child.id);
      node.childrenLoaded = true;
      node.hasChildren = children.length > 0;
    } catch {
      node.error = true;
    } finally {
      node.loading = false;
      renderTagTree();
    }
  }

  function toggleTagNode(node) {
    if (node.expanded) {
      node.expanded = false;
      renderTagTree();
      return;
    }
    if (node.childrenLoaded) {
      node.expanded = true;
      renderTagTree();
      return;
    }
    loadTagChildren(node.id, true);
  }

  function buildTagSearchTree(matches) {
    const roots = [];
    const views = new Map();
    for (const match of matches) {
      const chain = [...(match.ancestors || []), match.node].filter(Boolean);
      let parent = null;
      chain.forEach((raw, index) => {
        const node = rememberTagNode(raw);
        let view = views.get(node.id);
        if (!view) {
          view = { id: node.id, children: [], alias: "" };
          views.set(node.id, view);
        }
        if (index === chain.length - 1 && match.matchedAlias) {
          view.alias = match.matchedName || "";
        }
        if (parent) {
          if (!parent.children.includes(view)) {
            parent.children.push(view);
          }
        } else if (!roots.includes(view)) {
          roots.push(view);
        }
        parent = view;
      });
    }
    return roots;
  }

  function exitTagTreeSearch() {
    tagTreeSearchActive = false;
    tagTreeSearchRoots = [];
    tagTreeSearchLoading = false;
    tagTreeSearchError = false;
    tagTreeSearchExpanded = new Map();
    tagTreeSearchToken += 1;
    if (tagTreeSearchController) {
      tagTreeSearchController.abort();
      tagTreeSearchController = null;
    }
    renderTagTree();
  }

  async function runTagTreeSearch() {
    const text = tagTreeSearch.value.trim();
    tagTreeSearchText = text;
    if (tagTreeSearchController) {
      tagTreeSearchController.abort();
      tagTreeSearchController = null;
    }
    tagTreeSearchToken += 1;
    const token = tagTreeSearchToken;

    if (!text) {
      exitTagTreeSearch();
      return;
    }

    tagTreeSearchActive = true;
    tagTreeSearchLoading = true;
    tagTreeSearchError = false;
    tagTreeSearchRoots = [];
    tagTreeSearchExpanded = new Map();
    renderTagTree();

    const controller = new AbortController();
    tagTreeSearchController = controller;
    try {
      const data = await requestGraphQl(
        TAG_TREE_SEARCH_QUERY,
        { query: text },
        controller.signal,
      );
      if (token !== tagTreeSearchToken) {
        return;
      }
      tagTreeSearchRoots = buildTagSearchTree(data.tagTreeSearch || []);
    } catch (error) {
      if (error.name === "AbortError" || token !== tagTreeSearchToken) {
        return;
      }
      tagTreeSearchError = true;
    } finally {
      if (token === tagTreeSearchToken) {
        tagTreeSearchLoading = false;
        tagTreeSearchController = null;
        renderTagTree();
      }
    }
  }

  function scheduleTagTreeSearch() {
    clearTimeout(tagTreeSearchTimer);
    tagTreeSearchClear.hidden = tagTreeSearch.value === "";
    if (tagTreeSearch.value.trim() === "") {
      exitTagTreeSearch();
      return;
    }
    tagTreeSearchTimer = window.setTimeout(runTagTreeSearch, TAG_TREE_SEARCH_DELAY);
  }

  function setTagTreeStatus(text, options = {}) {
    tagTreeStatus.replaceChildren();
    tagTreeStatus.hidden = false;
    tagTreeStatus.classList.toggle("error", Boolean(options.error));
    tagTreeStatus.textContent = text;
    if (options.retry) {
      const retry = document.createElement("button");
      retry.type = "button";
      retry.className = "tag-tree-retry";
      retry.textContent = "重试";
      retry.addEventListener("click", options.retry);
      tagTreeStatus.append(" ", retry);
    }
  }

  function createTagTreeNote(text, depth, options = {}) {
    const note = document.createElement("div");
    note.className = "tag-tree-note";
    note.style.setProperty("--tree-depth", String(depth));
    note.classList.toggle("error", Boolean(options.error));
    note.textContent = text;
    if (options.retry) {
      const retry = document.createElement("button");
      retry.type = "button";
      retry.textContent = "重试";
      retry.addEventListener("click", options.retry);
      note.append(" ", retry);
    }
    return note;
  }

  function handleTagTreeActivate(node, event) {
    if (event.altKey) {
      const kind = event.shiftKey ? "exclude" : "include";
      const list = kind === "include" ? includeTags : excludeTags;
      const exists = list.some((tag) => tag.id === node.id);
      if (exists) {
        removeStageTag(node.id, kind);
      } else {
        addStageTag({ id: node.id, name: node.name, path: node.path }, kind);
      }
      return;
    }
    selectedTagId = selectedTagId === node.id ? "" : node.id;
    updateTagEditButton();
    renderTagTree();
  }

  function handleTagTreeKeydown(node, event, config) {
    if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      handleTagTreeActivate(node, event);
      return;
    }
    if (event.key === "ArrowRight" && config.hasChildren && !config.expanded) {
      event.preventDefault();
      config.onToggle();
      return;
    }
    if (event.key === "ArrowLeft" && config.hasChildren && config.expanded) {
      event.preventDefault();
      config.onToggle();
    }
  }

  function createTagTreeRow(node, depth, config) {
    const row = document.createElement("div");
    row.className = "tag-tree-row";
    row.dataset.tagId = node.id;
    row.style.setProperty("--tree-depth", String(depth));
    row.setAttribute("role", "treeitem");
    row.setAttribute("aria-level", String(depth + 1));
    row.classList.toggle("selected", selectedTagId === node.id);
    if (config.hasChildren) {
      row.setAttribute("aria-expanded", config.expanded ? "true" : "false");
    }

    if (config.hasChildren) {
      const arrow = document.createElement("button");
      arrow.type = "button";
      arrow.className = "tag-tree-arrow";
      arrow.setAttribute("aria-label", config.expanded ? "收起子标签" : "展开子标签");
      arrow.textContent = config.expanded ? "▾" : "▸";
      arrow.addEventListener("click", (event) => {
        event.stopPropagation();
        config.onToggle();
      });
      row.append(arrow);
    } else {
      const spacer = document.createElement("span");
      spacer.className = "tag-tree-arrow-spacer";
      spacer.setAttribute("aria-hidden", "true");
      row.append(spacer);
    }

    const label = document.createElement("button");
    label.type = "button";
    label.className = "tag-tree-label";
    label.dataset.tagId = node.id;
    label.title = node.path;
    const name = document.createElement("span");
    name.className = "tag-tree-name";
    name.textContent = node.name;
    label.append(name);
    if (config.alias) {
      const alias = document.createElement("span");
      alias.className = "tag-tree-alias";
      alias.textContent = config.alias;
      label.append(alias);
    }
    label.addEventListener("click", (event) => {
      handleTagTreeActivate(node, event);
    });
    label.addEventListener("keydown", (event) => {
      handleTagTreeKeydown(node, event, config);
    });
    row.append(label);
    return row;
  }

  function appendLazyTagBranch(fragment, id, depth) {
    const node = tagNodeCache.get(id);
    if (!node) {
      return;
    }
    const row = createTagTreeRow(node, depth, {
      hasChildren: node.hasChildren,
      expanded: node.expanded,
      alias: "",
      onToggle: () => toggleTagNode(node),
    });
    fragment.append(row);
    if (!node.expanded) {
      return;
    }

    const group = document.createElement("div");
    group.setAttribute("role", "group");
    if (node.loading) {
      group.append(createTagTreeNote("加载中…", depth + 1));
    } else if (node.error) {
      group.append(
        createTagTreeNote("加载失败", depth + 1, {
          error: true,
          retry: () => loadTagChildren(node.id, true),
        }),
      );
    } else if (node.childrenLoaded && node.children.length === 0) {
      group.append(createTagTreeNote("没有子标签", depth + 1));
    } else {
      for (const childId of node.children) {
        appendLazyTagBranch(group, childId, depth + 1);
      }
    }
    fragment.append(group);
  }

  function toggleSearchTagView(view) {
    const expanded = tagTreeSearchExpanded.get(view.id) !== false;
    tagTreeSearchExpanded.set(view.id, !expanded);
    renderTagTree();
  }

  function appendSearchTagBranch(fragment, view, depth) {
    const node = tagNodeCache.get(view.id);
    if (!node) {
      return;
    }
    const expanded = tagTreeSearchExpanded.get(view.id) !== false;
    const hasChildren = view.children.length > 0;
    const row = createTagTreeRow(node, depth, {
      hasChildren,
      expanded,
      alias: view.alias,
      onToggle: () => toggleSearchTagView(view),
    });
    fragment.append(row);
    if (!hasChildren || !expanded) {
      return;
    }
    const group = document.createElement("div");
    group.setAttribute("role", "group");
    for (const child of view.children) {
      appendSearchTagBranch(group, child, depth + 1);
    }
    fragment.append(group);
  }

  function renderTagTree() {
    tagTree.replaceChildren();
    tagTreeStatus.hidden = true;
    tagTreeStatus.classList.remove("error");
    tagTreeStatus.replaceChildren();

    if (tagTreeSearchActive) {
      if (tagTreeSearchLoading) {
        setTagTreeStatus("搜索中…");
        return;
      }
      if (tagTreeSearchError) {
        setTagTreeStatus("搜索失败", {
          error: true,
          retry: () => runTagTreeSearch(),
        });
        return;
      }
      if (tagTreeSearchRoots.length === 0) {
        setTagTreeStatus("没有匹配的标签");
        return;
      }
      const fragment = document.createDocumentFragment();
      for (const view of tagTreeSearchRoots) {
        appendSearchTagBranch(fragment, view, 0);
      }
      tagTree.append(fragment);
      return;
    }

    if (!tagTreeRootsLoaded) {
      if (tagTreeRootsError) {
        setTagTreeStatus("标签树加载失败", {
          error: true,
          retry: () => loadTagRoots(),
        });
      } else {
        setTagTreeStatus("加载中…");
      }
      return;
    }
    if (tagTreeRootIds.length === 0) {
      setTagTreeStatus("没有标签");
      return;
    }
    const fragment = document.createDocumentFragment();
    for (const id of tagTreeRootIds) {
      appendLazyTagBranch(fragment, id, 0);
    }
    tagTree.append(fragment);
  }

  function updateTagEditButton() {
    const node = selectedTagId ? tagNodeCache.get(selectedTagId) : null;
    tagEditButton.title = node ? `编辑标签：${node.path}` : "添加顶层标签";
  }

  function setTagDrawerOpen(open) {
    if (tagDrawerOpen === open) {
      return;
    }
    tagDrawerOpen = open;
    document.body.classList.toggle("tag-drawer-open", open);
    tagDrawerHandle.setAttribute("aria-expanded", open ? "true" : "false");
    tagDrawerPanel.inert = !open;
    if (open) {
      if (!tagTreeRootsLoaded && !tagTreeRootsLoading) {
        loadTagRoots();
      } else {
        renderTagTree();
      }
    }
    if (layoutAuto) {
      syncLayoutSettings();
    }
    if (window.innerWidth >= 900) {
      window.setTimeout(relayout, 240);
    }
  }

  function tagPathFor(name, parentId, parentNode, fallbackNode) {
    if (!parentId) {
      return name;
    }
    const parent = parentNode || tagNodeCache.get(parentId);
    if (parent && parent.path) {
      return `${parent.path}::${name}`;
    }
    if (
      fallbackNode &&
      fallbackNode.parentId === parentId &&
      fallbackNode.path
    ) {
      const parts = fallbackNode.path.split("::");
      parts[parts.length - 1] = name;
      return parts.join("::");
    }
    return name;
  }

  function syncStageTagPaths(saved, oldPath, newPath) {
    for (const list of [includeTags, excludeTags]) {
      for (const tag of list) {
        if (tag.id === saved.id) {
          tag.name = saved.name;
          tag.path = newPath;
        } else if (oldPath && tag.path && tag.path.startsWith(`${oldPath}::`)) {
          tag.path = `${newPath}${tag.path.slice(oldPath.length)}`;
        }
      }
    }
    renderStaging();
    syncStateToUrl();
  }

  async function findTagTreeMatch(id, name) {
    const data = await requestGraphQl(TAG_TREE_SEARCH_QUERY, { query: name });
    return (data.tagTreeSearch || []).find((match) => match.node.id === id);
  }

  async function refreshTagTreeAfterSave(saved) {
    resetTagTree();
    await loadTagRoots();
    try {
      const match = await findTagTreeMatch(saved.id, saved.name);
      if (match) {
        const chain = [...(match.ancestors || []), match.node];
        for (let index = 0; index < chain.length - 1; index += 1) {
          rememberTagNode(chain[index]);
          await loadTagChildren(chain[index].id, true);
        }
        rememberTagNode(match.node);
      }
    } catch {
      // 树刷新失败时保留已加载的根节点，不影响后续手动展开。
    }
    if (tagTreeSearchActive) {
      await runTagTreeSearch();
    }
    if (tagNodeCache.has(saved.id)) {
      selectedTagId = saved.id;
    }
    updateTagEditButton();
    renderTagTree();
  }

  function isTagEditorSelfOrDescendant(path) {
    const node = tagEditorTagId ? tagNodeCache.get(tagEditorTagId) : null;
    if (!node || !path) {
      return false;
    }
    return path === node.path || path.startsWith(`${node.path}::`);
  }

  function showTagEditorError(message) {
    tagEditorError.textContent = message;
    tagEditorError.hidden = false;
  }

  function clearTagEditorError() {
    tagEditorError.textContent = "";
    tagEditorError.hidden = true;
  }

  function renderTagEditorAliases() {
    tagEditorAliases.replaceChildren();
    for (const alias of tagEditorAliasesValue) {
      const chip = document.createElement("span");
      chip.className = "tag-editor-alias";
      const text = document.createElement("span");
      text.textContent = alias;
      const remove = document.createElement("button");
      remove.type = "button";
      remove.className = "tag-editor-alias-remove";
      remove.setAttribute("aria-label", `删除别名 ${alias}`);
      remove.textContent = "×";
      remove.addEventListener("click", () => {
        tagEditorAliasesValue = tagEditorAliasesValue.filter(
          (value) => value !== alias,
        );
        renderTagEditorAliases();
        clearTagEditorError();
      });
      chip.append(text, remove);
      tagEditorAliases.append(chip);
    }
    tagEditorAliases.hidden = tagEditorAliasesValue.length === 0;
  }

  function addTagEditorAliases(raw) {
    const standard = tagEditorName.value.trim().toLowerCase();
    for (const part of raw.split(/[,，]/)) {
      const alias = part.trim();
      if (!alias || alias.toLowerCase() === standard) {
        continue;
      }
      if (
        tagEditorAliasesValue.some(
          (value) => value.toLowerCase() === alias.toLowerCase(),
        )
      ) {
        continue;
      }
      tagEditorAliasesValue.push(alias);
    }
    renderTagEditorAliases();
  }

  function commitTagEditorAliasInput() {
    if (!tagEditorAliasInput.value.trim()) {
      return;
    }
    addTagEditorAliases(tagEditorAliasInput.value);
    tagEditorAliasInput.value = "";
    clearTagEditorError();
  }

  function renderTagEditor() {
    tagEditorTitle.textContent =
      tagEditorMode === "edit" ? "编辑标签" : "添加子标签";
    tagEditorModeEdit.disabled = !tagEditorTagId;
    tagEditorModeEdit.setAttribute(
      "aria-selected",
      tagEditorMode === "edit" ? "true" : "false",
    );
    tagEditorModeAdd.setAttribute(
      "aria-selected",
      tagEditorMode === "add" ? "true" : "false",
    );

    tagEditorParentDisplay.textContent = tagEditorParentNode
      ? tagEditorParentNode.path
      : "全局父节点";
    tagEditorParentDisplay.title = tagEditorParentDisplay.textContent;
    tagEditorParentDisplay.disabled = tagEditorMode === "add";
    tagEditorParentClear.hidden =
      tagEditorMode === "add" || tagEditorParentId === null;
    tagEditorParentSearch.hidden = true;

    renderTagEditorAliases();
    tagEditorSave.disabled = tagEditorSaving;
    tagEditorSave.textContent = tagEditorSaving ? "保存中…" : "保存";
  }

  function setTagEditorMode(mode) {
    if (mode === "edit" && !tagEditorTagId) {
      return;
    }
    tagEditorMode = mode;
    tagEditorParentSearch.hidden = true;
    tagEditorParentInput.value = "";
    if (tagEditorParentSuggestWidget) {
      tagEditorParentSuggestWidget.clearChosen();
      tagEditorParentSuggestWidget.close();
    }
    if (mode === "add") {
      tagEditorParentId = tagEditorTagId || null;
      tagEditorParentNode = tagEditorParentId
        ? tagNodeCache.get(tagEditorParentId)
        : null;
    } else {
      tagEditorParentId = tagEditorOriginalParentId;
      tagEditorParentNode = tagEditorParentId
        ? tagNodeCache.get(tagEditorParentId)
        : null;
    }
    clearTagEditorError();
    renderTagEditor();
  }

  function openTagEditor() {
    if (tagEditorOpen) {
      return;
    }
    const selected = selectedTagId
      ? tagNodeCache.get(selectedTagId)
      : null;
    tagEditorTagId = selected ? selected.id : "";
    tagEditorMode = selected ? "edit" : "add";
    tagEditorAliasesValue = selected ? [...selected.aliases] : [];
    tagEditorOriginalParentId = selected ? selected.parentId : null;
    tagEditorParentId =
      tagEditorMode === "add"
        ? selected
          ? selected.id
          : null
        : selected
          ? selected.parentId
          : null;
    tagEditorParentNode = tagEditorParentId
      ? tagNodeCache.get(tagEditorParentId)
      : null;
    tagEditorName.value = selected ? selected.name : "";
    tagEditorAliasInput.value = "";
    if (tagEditorParentSuggestWidget) {
      tagEditorParentSuggestWidget.clearChosen();
      tagEditorParentSuggestWidget.close();
    }
    clearTagEditorError();
    tagEditorOpen = true;
    renderTagEditor();
    tagEditor.showModal();
    tagEditorName.focus();
    tagEditorName.select();
  }

  function closeTagEditor() {
    tagEditorOpen = false;
    tagEditorSaving = false;
    tagEditorParentSearch.hidden = true;
    tagEditorParentInput.value = "";
    if (tagEditorParentSuggestWidget) {
      tagEditorParentSuggestWidget.clearChosen();
      tagEditorParentSuggestWidget.close();
    }
    clearTagEditorError();
    if (tagEditor.open) {
      tagEditor.close();
    }
  }

  function chooseTagEditorParent(tag) {
    if (!tag) {
      return false;
    }
    if (isTagEditorSelfOrDescendant(tag.path)) {
      showTagEditorError("不能把标签移动到自身或自己的后代下");
      return false;
    }
    tagEditorParentId = tag.id;
    tagEditorParentNode =
      tagNodeCache.get(tag.id) ||
      rememberTagNode({
        id: tag.id,
        parentId: null,
        name: tag.name,
        aliases: [],
        path: tag.path,
        hasChildren: false,
      });
    tagEditorParentSearch.hidden = true;
    tagEditorParentInput.value = "";
    if (tagEditorParentSuggestWidget) {
      tagEditorParentSuggestWidget.clearChosen();
      tagEditorParentSuggestWidget.close();
    }
    clearTagEditorError();
    renderTagEditor();
    return true;
  }

  function openTagEditorParentSearch() {
    if (tagEditorMode !== "edit" || !tagEditorTagId) {
      return;
    }
    tagEditorParentSearch.hidden = false;
    tagEditorParentInput.value = "";
    if (tagEditorParentSuggestWidget) {
      tagEditorParentSuggestWidget.clearChosen();
      tagEditorParentSuggestWidget.close();
    }
    tagEditorParentInput.focus();
  }

  function clearTagEditorParent() {
    tagEditorParentId = null;
    tagEditorParentNode = null;
    tagEditorParentSearch.hidden = true;
    tagEditorParentInput.value = "";
    clearTagEditorError();
    renderTagEditor();
  }

  async function saveTagEditor() {
    if (tagEditorSaving) {
      return;
    }
    commitTagEditorAliasInput();
    const name = tagEditorName.value.trim();
    if (!name) {
      showTagEditorError("请输入标签标准名");
      tagEditorName.focus();
      return;
    }
    if (
      tagEditorAliasesValue.some(
        (alias) => alias.toLowerCase() === name.toLowerCase(),
      )
    ) {
      showTagEditorError("别名不能与标准名相同");
      tagEditorAliasInput.focus();
      return;
    }

    const oldNode = tagEditorTagId
      ? tagNodeCache.get(tagEditorTagId)
      : null;
    const oldPath = oldNode ? oldNode.path : "";
    const parentNode = tagEditorParentId
      ? tagEditorParentNode || tagNodeCache.get(tagEditorParentId)
      : null;

    tagEditorSaving = true;
    clearTagEditorError();
    renderTagEditor();
    try {
      let saved = null;
      if (tagEditorMode === "add") {
        const data = await requestGraphQl(CREATE_TAG_MUTATION, {
          input: {
            name,
            aliases: tagEditorAliasesValue,
            parentId: tagEditorParentId,
          },
        });
        saved = data.createTag;
      } else {
        const input = { name, aliases: tagEditorAliasesValue };
        if (
          tagEditorParentId === null &&
          tagEditorOriginalParentId !== null
        ) {
          input.clearParent = true;
        } else if (
          tagEditorParentId &&
          tagEditorParentId !== tagEditorOriginalParentId
        ) {
          input.parentId = tagEditorParentId;
        }
        const data = await requestGraphQl(UPDATE_TAG_MUTATION, {
          id: tagEditorTagId,
          input,
        });
        saved = data.updateTag;
      }
      if (!saved) {
        throw new Error("保存失败");
      }
      const finalPath = tagPathFor(
        saved.name,
        tagEditorParentId,
        parentNode,
        oldNode,
      );
      closeTagEditor();
      syncStageTagPaths(saved, oldPath, finalPath);
      await refreshTagTreeAfterSave(saved);
    } catch (error) {
      showTagEditorError(error.message || "保存失败");
    } finally {
      tagEditorSaving = false;
      if (tagEditorOpen) {
        renderTagEditor();
      }
    }
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

    const rawIncludeMode = (params.get("includeMode") || "")
      .trim()
      .toLowerCase();
    includeMode =
      rawIncludeMode === "any" && includeTags.length > 0 ? "ANY" : "ALL";

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
    if (includeMode === "ANY" && includeTags.length > 0) {
      params.set("includeMode", "any");
    }
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
      filter.includeMode = includeMode;
      filter.expandIncludeTagDescendants = true;
    }
    if (excludeTags.length > 0) {
      filter.excludeTagIds = excludeTags.map((tag) => tag.id);
      filter.expandExcludeTagDescendants = true;
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
          // 默认排除标签由后端注入；即使当前没有筛选条件，也要让后端
          // 递归排除配置标签的整棵子树，而不是只排除标签本身。
          filter: { ...(filter || {}), expandExcludeTagDescendants: true },
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
    // 桌面抽屉会推挤内容，可用宽度要扣掉抽屉；窄屏抽屉是覆盖层，不扣。
    const drawerWidth =
      (tagDrawerOpen ? TAG_DRAWER_WIDTH : 0) +
      (workbench.open ? WORKBENCH_WIDTH : 0);
    const width =
      window.innerWidth - (window.innerWidth >= 900 ? drawerWidth : 0);
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
    image.draggable = false;
    image.addEventListener("dragstart", (event) => event.preventDefault());
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
    card.dataset.imageId = item.id;

    const width = Number(item.width);
    const height = Number(item.height);
    const ratioWidth = width > 0 ? width : 3;
    const ratioHeight = height > 0 ? height : 2;
    card.dataset.aspectRatio = String(ratioWidth / ratioHeight);

    const button = document.createElement("button");
    button.type = "button";
    button.className = "image-thumb-button";
    button.setAttribute("aria-pressed", "false");
    button.setAttribute(
      "aria-label",
      `查看大图：${artworkTitles(item).join("、") || "图片"}`,
    );

    const thumb = createThumb(item);
    if (isSimilarityActive()) {
      thumb.appendChild(createScoreBadge(item));
    }
    button.appendChild(thumb);
    button.addEventListener("click", (event) =>
      handleCardClick(item, index, event),
    );

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
    applySelectionToCard(card);
    syncWorkbenchImage(card);
    return card;
  }

  // ---------- 多选批量标签 ----------

  function multiSelectEnabled() {
    if (workbench.open) {
      return false;
    }
    if (window.innerWidth < 900) {
      return false;
    }
    if (
      typeof window.matchMedia === "function" &&
      window.matchMedia("(hover: none)").matches
    ) {
      return false;
    }
    return true;
  }

  function selectionHasChanges() {
    return (
      selectionPendingAdditions.length > 0 ||
      selectionPendingRemovals.size > 0
    );
  }

  function selectionLocked() {
    return selectionSaving || selectionHasChanges();
  }

  function getSelectionCommonTags() {
    if (selectedImageItems.size === 0) {
      return [];
    }
    let common = null;
    for (const item of selectedImageItems.values()) {
      const tags = new Map();
      for (const tag of item.tags || []) {
        tags.set(tag.id, tag);
      }
      if (common === null) {
        common = tags;
      } else {
        for (const id of [...common.keys()]) {
          if (!tags.has(id)) {
            common.delete(id);
          }
        }
      }
      if (common.size === 0) {
        break;
      }
    }
    return [...(common ? common.values() : [])].sort((left, right) =>
      String(left.name).localeCompare(String(right.name), "zh-CN"),
    );
  }

  function getSelectionTagPaths() {
    const paths = new Map();
    for (const item of selectedImageItems.values()) {
      for (const entry of item.tagPaths || []) {
        if (!paths.has(entry.id)) {
          paths.set(entry.id, entry.path);
        }
      }
    }
    return paths;
  }

  function setSelectionError(text) {
    const value = String(text || "");
    selectionError.textContent = value;
    selectionError.hidden = value === "";
  }

  function clearSelectionError() {
    setSelectionError("");
  }

  function updateSelectionUi() {
    const count = selectedImageItems.size;
    selectionCount.textContent = String(count);
    const hasChanges = selectionHasChanges();
    selectionSave.disabled = !hasChanges || selectionSaving;
    selectionSave.textContent = selectionSaving ? "保存中…" : "保存";
    selectionCancel.disabled = selectionSaving;
    selectionClose.disabled = selectionSaving;
    selectionTagInput.disabled = selectionSaving;
    selectionLockHint.hidden = !(hasChanges || selectionSaving);
    selectionLockHint.textContent = selectionSaving
      ? "保存中…"
      : "保存或取消后可继续选择";
    selectionCard.hidden = count === 0;
  }

  function renderSelectionTags() {
    const fragment = document.createDocumentFragment();
    const common = getSelectionCommonTags();
    const paths = getSelectionTagPaths();

    if (common.length === 0) {
      const empty = document.createElement("span");
      empty.className = "selection-empty";
      empty.textContent = "没有共同标签";
      fragment.appendChild(empty);
    }

    for (const tag of common) {
      const path = paths.get(tag.id) || tag.path || tag.name;
      const removing = selectionPendingRemovals.has(tag.id);
      const chip = document.createElement("button");
      chip.type = "button";
      chip.className = "chip chip-editable";
      if (removing) {
        chip.classList.add("chip-remove");
      }
      chip.textContent = tag.name;
      chip.title = `${path}\n${removing ? "点击取消删除" : "点击标记删除"}`;
      chip.setAttribute("aria-pressed", String(removing));
      chip.addEventListener("click", () => toggleSelectionRemoval(tag.id));
      fragment.appendChild(chip);
    }

    for (const tag of selectionPendingAdditions) {
      const chip = document.createElement("button");
      chip.type = "button";
      chip.className = "chip chip-editable chip-add";
      chip.textContent = tag.name;
      chip.title = `${tag.path || tag.name}\n点击移除`;
      chip.addEventListener("click", () => removeSelectionAddition(tag.id));
      fragment.appendChild(chip);
    }

    selectionCommonTags.replaceChildren(fragment);
  }

  function selectionChanged() {
    if (selectedImageItems.size === 0) {
      selectionCount.textContent = "0";
      resetSelectionEditor();
      selectionCard.hidden = true;
      return;
    }
    selectionCard.hidden = false;
    renderSelectionTags();
    updateSelectionUi();
  }

  function applySelectionToCard(card) {
    const selected = selectedImageItems.has(card.dataset.imageId);
    card.classList.toggle("is-selected", selected);
    const button = card.querySelector(".image-thumb-button");
    if (button) {
      button.setAttribute("aria-pressed", String(selected));
    }
  }

  function setImageSelected(item, selected) {
    if (!item || !item.id) {
      return;
    }
    if (selected) {
      selectedImageItems.set(item.id, item);
    } else {
      selectedImageItems.delete(item.id);
    }
    for (const card of allCards) {
      if (card.dataset.imageId === item.id) {
        applySelectionToCard(card);
      }
    }
    selectionChanged();
  }

  function addSelection(item) {
    if (!multiSelectEnabled() || !item || selectedImageItems.has(item.id)) {
      return;
    }
    if (selectionLocked()) {
      updateSelectionUi();
      return;
    }
    setImageSelected(item, true);
  }

  function toggleSelection(item) {
    if (!multiSelectEnabled() || !item) {
      return;
    }
    if (selectionLocked()) {
      updateSelectionUi();
      return;
    }
    setImageSelected(item, !selectedImageItems.has(item.id));
  }

  function resetSelectionEditor() {
    selectionPendingAdditions = [];
    selectionPendingRemovals.clear();
    clearSelectionError();
    selectionTagInput.value = "";
    selectionLockHint.hidden = true;
    selectionSave.disabled = true;
    if (selectionTagSuggestWidget) {
      selectionTagSuggestWidget.clearChosen();
      selectionTagSuggestWidget.close();
    }
  }

  function clearSelection() {
    finishSelectionDrag(false);
    selectedImageItems.clear();
    for (const card of allCards) {
      applySelectionToCard(card);
    }
    selectionSaving = false;
    resetSelectionEditor();
    selectionCount.textContent = "0";
    selectionCard.hidden = true;
  }

  function toggleSelectionRemoval(id) {
    if (selectionSaving) {
      return;
    }
    if (selectionPendingRemovals.has(id)) {
      selectionPendingRemovals.delete(id);
    } else {
      selectionPendingRemovals.add(id);
    }
    clearSelectionError();
    renderSelectionTags();
    updateSelectionUi();
  }

  function removeSelectionAddition(id) {
    if (selectionSaving) {
      return;
    }
    selectionPendingAdditions = selectionPendingAdditions.filter(
      (tag) => tag.id !== id,
    );
    clearSelectionError();
    renderSelectionTags();
    updateSelectionUi();
  }

  function addSelectionTag(tag) {
    if (!tag || !tag.id || selectionSaving) {
      return false;
    }
    clearSelectionError();
    const commonIds = new Set(getSelectionCommonTags().map((item) => item.id));
    if (commonIds.has(tag.id)) {
      selectionPendingRemovals.delete(tag.id);
    } else if (
      !selectionPendingAdditions.some((item) => item.id === tag.id)
    ) {
      selectionPendingAdditions.push({
        id: tag.id,
        name: tag.name,
        path: tag.path || tag.name,
      });
    }
    renderSelectionTags();
    updateSelectionUi();
    return true;
  }

  // Shift+回车：移除待添加标签，或给公共标签打上删除标记。
  function removeSelectionTag(tag) {
    if (!tag || !tag.id || selectionSaving) {
      return false;
    }
    clearSelectionError();
    const pendingIndex = selectionPendingAdditions.findIndex(
      (item) => item.id === tag.id,
    );
    if (pendingIndex >= 0) {
      selectionPendingAdditions.splice(pendingIndex, 1);
      renderSelectionTags();
      updateSelectionUi();
      return true;
    }

    const commonIds = new Set(getSelectionCommonTags().map((item) => item.id));
    if (commonIds.has(tag.id)) {
      selectionPendingRemovals.add(tag.id);
      renderSelectionTags();
      updateSelectionUi();
    }
    return true;
  }

  function applySelectionTagChangesLocally(imageIds, additions, removeIds) {
    const idSet = new Set(imageIds);
    const removeSet = new Set(removeIds);
    const itemById = new Map();
    for (const item of allItems) {
      if (idSet.has(item.id)) {
        itemById.set(item.id, item);
      }
    }

    for (const item of itemById.values()) {
      const tags = (item.tags || []).filter((tag) => !removeSet.has(tag.id));
      for (const tag of additions) {
        if (!tags.some((entry) => entry.id === tag.id)) {
          tags.push({
            id: tag.id,
            name: tag.name,
            category: tag.category || null,
            aliases: Array.isArray(tag.aliases) ? tag.aliases : [],
          });
        }
      }
      item.tags = tags;

      const paths = (item.tagPaths || []).filter(
        (entry) => !removeSet.has(entry.id),
      );
      for (const tag of additions) {
        if (!paths.some((entry) => entry.id === tag.id)) {
          paths.push({ id: tag.id, path: tag.path || tag.name });
        }
      }
      item.tagPaths = paths;
    }

    for (const card of allCards) {
      const item = itemById.get(card.dataset.imageId);
      if (!item) {
        continue;
      }
      const body = card.querySelector(".card-body");
      if (!body) {
        continue;
      }
      const oldTags = body.querySelector(".tags");
      if (oldTags) {
        oldTags.remove();
      }
      const tags = createTagList(item);
      if (tags.childElementCount > 0) {
        body.appendChild(tags);
      }
    }
  }

  async function saveSelection() {
    if (
      selectionSaving ||
      selectedImageItems.size === 0 ||
      !selectionHasChanges()
    ) {
      return;
    }

    const imageIds = [...selectedImageItems.keys()];
    const additions = selectionPendingAdditions.map((tag) => ({ ...tag }));
    const removeIds = [...selectionPendingRemovals];
    const hadTagFilter = includeTags.length > 0 || excludeTags.length > 0;

    selectionSaving = true;
    clearSelectionError();
    updateSelectionUi();

    try {
      await requestGraphQl(UPDATE_IMAGES_MUTATION, {
        input: {
          imageIds,
          addTagIds: additions.map((tag) => tag.id),
          removeTagIds: removeIds,
        },
      });
      applySelectionTagChangesLocally(imageIds, additions, removeIds);
      clearSelection();
      if (hadTagFilter) {
        search(true);
      }
    } catch (error) {
      selectionSaving = false;
      updateSelectionUi();
      setSelectionError(error.message || "保存失败");
    }
  }

  function handleCardClick(item, index, event) {
    if (suppressSelectionClick) {
      suppressSelectionClick = false;
      window.clearTimeout(suppressSelectionClickTimer);
      event.preventDefault();
      return;
    }
    if (workbench.open) {
      if (event.ctrlKey || event.metaKey || event.altKey) {
        event.preventDefault();
        return;
      }
      openLightbox(index);
      return;
    }
    if (!multiSelectEnabled()) {
      openLightbox(index);
      return;
    }
    if (
      selectedImageItems.size > 0 ||
      event.ctrlKey ||
      event.metaKey
    ) {
      event.preventDefault();
      toggleSelection(item);
      return;
    }
    openLightbox(index);
  }

  function selectCardAtPoint(x, y, event) {
    let target = null;
    if (typeof document.elementFromPoint === "function") {
      target = document.elementFromPoint(x, y);
    }
    if (!(target instanceof Element) && event) {
      target = event.target instanceof Element ? event.target : null;
    }
    if (!target) {
      return;
    }
    const card = target.closest(".image-card");
    if (!card || !results.contains(card)) {
      return;
    }
    const item = allItems.find((entry) => entry.id === card.dataset.imageId);
    if (item) {
      addSelection(item);
    }
  }

  function tickSelectionAutoScroll() {
    selectionDrag.scrollFrame = 0;
    if (!selectionDrag.active || selectionDrag.scrollSpeed === 0) {
      return;
    }
    window.scrollBy(0, selectionDrag.scrollSpeed);
    selectCardAtPoint(selectionDrag.lastX, selectionDrag.lastY);
    selectionDrag.scrollFrame = window.requestAnimationFrame(
      tickSelectionAutoScroll,
    );
  }

  function updateSelectionAutoScroll(y) {
    const margin = 72;
    let speed = 0;
    if (y < margin) {
      speed = -Math.ceil((margin - y) / 4);
    } else if (y > window.innerHeight - margin) {
      speed = Math.ceil((y - (window.innerHeight - margin)) / 4);
    }
    speed = Math.max(-45, Math.min(45, speed));
    selectionDrag.scrollSpeed = speed;

    if (speed !== 0 && selectionDrag.scrollFrame === 0) {
      selectionDrag.scrollFrame = window.requestAnimationFrame(
        tickSelectionAutoScroll,
      );
    } else if (speed === 0 && selectionDrag.scrollFrame !== 0) {
      window.cancelAnimationFrame(selectionDrag.scrollFrame);
      selectionDrag.scrollFrame = 0;
    }
  }

  function finishSelectionDrag(suppressClick) {
    if (!selectionDrag.active) {
      return;
    }
    const moved = selectionDrag.moved;
    selectionDrag.active = false;
    selectionDrag.pointerId = null;
    selectionDrag.moved = false;
    selectionDrag.scrollSpeed = 0;
    if (selectionDrag.scrollFrame !== 0) {
      window.cancelAnimationFrame(selectionDrag.scrollFrame);
      selectionDrag.scrollFrame = 0;
    }
    document.body.classList.remove("selection-dragging");
    if (moved && suppressClick) {
      suppressSelectionClick = true;
      window.clearTimeout(suppressSelectionClickTimer);
      suppressSelectionClickTimer = window.setTimeout(() => {
        suppressSelectionClick = false;
      }, 0);
    }
  }

  function beginSelectionDrag(event) {
    if (workbench.open) {
      return;
    }
    if (
      !multiSelectEnabled() ||
      event.button !== 0 ||
      event.pointerType === "touch" ||
      !(event.ctrlKey || event.metaKey)
    ) {
      return;
    }
    if (selectionLocked()) {
      updateSelectionUi();
      return;
    }
    finishSelectionDrag(false);
    selectionDrag.active = true;
    selectionDrag.pointerId =
      typeof event.pointerId === "number" ? event.pointerId : null;
    selectionDrag.startX = event.clientX;
    selectionDrag.startY = event.clientY;
    selectionDrag.lastX = event.clientX;
    selectionDrag.lastY = event.clientY;
    selectionDrag.moved = false;
    document.body.classList.add("selection-dragging");
    exitHoverPreview();
  }

  function bindSelectionUi() {
    selectionClose.addEventListener("click", clearSelection);
    selectionCancel.addEventListener("click", clearSelection);
    selectionSave.addEventListener("click", saveSelection);
    selectionTagInput.addEventListener("input", clearSelectionError);

    selectionTagSuggestWidget = createTagSearch({
      input: selectionTagInput,
      list: selectionTagSuggest,
      idPrefix: "selection-tag",
      onAdd: addSelectionTag,
      onRemove: removeSelectionTag,
      onError: setSelectionError,
    });

    results.addEventListener("pointerdown", beginSelectionDrag);
    document.addEventListener(
      "pointermove",
      (event) => {
        if (!selectionDrag.active) {
          return;
        }
        if (
          selectionDrag.pointerId !== null &&
          event.pointerId !== selectionDrag.pointerId
        ) {
          return;
        }
        selectionDrag.lastX = event.clientX;
        selectionDrag.lastY = event.clientY;
        if (!selectionDrag.moved) {
          const dx = event.clientX - selectionDrag.startX;
          const dy = event.clientY - selectionDrag.startY;
          if (Math.hypot(dx, dy) < 4) {
            return;
          }
          selectionDrag.moved = true;
          selectCardAtPoint(
            selectionDrag.startX,
            selectionDrag.startY,
            event,
          );
        }
        event.preventDefault();
        selectCardAtPoint(event.clientX, event.clientY, event);
        updateSelectionAutoScroll(event.clientY);
      },
      { passive: false },
    );
    document.addEventListener("pointerup", (event) => {
      if (!selectionDrag.active) {
        return;
      }
      if (
        selectionDrag.pointerId !== null &&
        event.pointerId !== selectionDrag.pointerId
      ) {
        return;
      }
      finishSelectionDrag(true);
    });
    document.addEventListener("pointercancel", (event) => {
      if (
        selectionDrag.active &&
        (selectionDrag.pointerId === null ||
          event.pointerId === selectionDrag.pointerId)
      ) {
        finishSelectionDrag(false);
      }
    });
    window.addEventListener("blur", () => finishSelectionDrag(false));

    document.addEventListener("keydown", (event) => {
      if (event.key !== "Escape" || event.defaultPrevented) {
        return;
      }
      if (ImageLightbox.isOpen() || tagEditorOpen) {
        return;
      }
      if (selectionDrag.active) {
        event.preventDefault();
        finishSelectionDrag(true);
        return;
      }
      if (selectionSaving) {
        event.preventDefault();
        return;
      }
      if (selectedImageItems.size > 0) {
        event.preventDefault();
        clearSelection();
      }
    });
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
      if (ImageLightbox.isOpen()) {
        ImageLightbox.close();
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

  function appendLightboxMeta(refs, term, value) {
    const dt = document.createElement("dt");
    dt.textContent = term;
    const dd = document.createElement("dd");
    dd.textContent = value;
    refs.meta.append(dt, dd);
  }

  function renderLightboxSidebar(entry, refs) {
    const item = entry.item || entry;
    refs.similar.href = buildSimilarUrl(item.id);

    const artworks = Array.isArray(item.artworks) ? item.artworks : [];
    if (artworks.length === 0) {
      const link = document.createElement("a");
      link.className = "image-artwork-link";
      link.href = "/orphans.html";
      link.textContent = "游离图片";
      refs.artworks.appendChild(link);
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

        row.append(link, meta);
        refs.artworks.appendChild(row);
      }
    }

    const tags = Array.isArray(item.tags) ? item.tags : [];
    if (tags.length === 0) {
      const empty = document.createElement("span");
      empty.className = "image-tag-empty";
      empty.textContent = "无标签";
      refs.tags.appendChild(empty);
    } else {
      const paths = pathMapOf(item);
      for (const tag of tags) {
        const chip = document.createElement("span");
        chip.className = "chip";
        chip.textContent = tag.name;
        chip.title = tagTooltip(tag, paths.get(tag.id));
        refs.tags.appendChild(chip);
      }
    }

    const width = Number(item.width);
    const height = Number(item.height);
    appendLightboxMeta(
      refs,
      "尺寸",
      width > 0 && height > 0 ? `${width} × ${height}` : "未知",
    );
    if (item.frameCount !== null && item.frameCount !== undefined) {
      appendLightboxMeta(refs, "帧数", String(item.frameCount));
    }
    appendLightboxMeta(refs, "创建时间", formatDate(item.createdAt) || "未知");
    appendLightboxMeta(
      refs,
      "扫描时间",
      item.scanAt ? formatDate(item.scanAt) : "从未扫描",
    );
    appendLightboxMeta(
      refs,
      "发布时间",
      item.publishedAt ? formatDate(item.publishedAt) : "未设置",
    );
    appendLightboxMeta(refs, "图片 ID", item.id);
  }

  ImageLightbox.init({ renderSidebar: renderLightboxSidebar });

  function openLightbox(index) {
    if (index < 0 || index >= allItems.length) {
      return;
    }
    exitHoverPreview();
    const entries = allItems.map((item, position) => ({
      id: item.id,
      alt: artworkTitles(item).join("、") || `图片 ${position + 1}`,
      item,
    }));
    ImageLightbox.open(entries, index);
  }

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
    if (event.pointerType === "touch" || selectionDrag.active) {
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
    clearSelection();
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
    if (
      event.key === "Escape" &&
      !event.defaultPrevented &&
      !layoutPanel.hidden &&
      selectedImageItems.size === 0 &&
      !selectionDrag.active &&
      !tagDrawerOpen &&
      !tagEditorOpen &&
      !ImageLightbox.isOpen()
    ) {
      setLayoutPanelOpen(false);
    }
  });
  document.addEventListener("pointerdown", (event) => {
    if (!layoutPanel.hidden && !layoutTools.contains(event.target)) {
      setLayoutPanelOpen(false);
    }
  });

  function bindTagTreeUi() {
    tagDrawerHandle.addEventListener("click", () =>
      setTagDrawerOpen(!tagDrawerOpen),
    );
    tagDrawerClose.addEventListener("click", () => setTagDrawerOpen(false));
    tagEditButton.addEventListener("click", openTagEditor);

    tagTreeSearch.addEventListener("input", scheduleTagTreeSearch);
    tagTreeSearchClear.addEventListener("click", () => {
      tagTreeSearch.value = "";
      tagTreeSearchClear.hidden = true;
      clearTimeout(tagTreeSearchTimer);
      tagTreeSearchText = "";
      exitTagTreeSearch();
      tagTreeSearch.focus();
    });

    tagEditorForm.addEventListener("submit", (event) => {
      event.preventDefault();
      saveTagEditor();
    });
    tagEditorSave.addEventListener("click", saveTagEditor);
    tagEditorClose.addEventListener("click", closeTagEditor);
    tagEditorCancel.addEventListener("click", closeTagEditor);
    tagEditorModeEdit.addEventListener("click", () =>
      setTagEditorMode("edit"),
    );
    tagEditorModeAdd.addEventListener("click", () =>
      setTagEditorMode("add"),
    );

    tagEditorName.addEventListener("input", clearTagEditorError);
    tagEditorName.addEventListener("keydown", (event) => {
      if (event.key === "Enter" && !event.isComposing) {
        event.preventDefault();
        saveTagEditor();
      }
    });
    tagEditorAliasInput.addEventListener("input", clearTagEditorError);
    tagEditorAliasInput.addEventListener("keydown", (event) => {
      if (
        event.key === "Enter" ||
        event.key === "," ||
        event.key === "，"
      ) {
        event.preventDefault();
        commitTagEditorAliasInput();
        return;
      }
      if (
        event.key === "Backspace" &&
        tagEditorAliasInput.value === "" &&
        tagEditorAliasesValue.length > 0
      ) {
        event.preventDefault();
        tagEditorAliasesValue.pop();
        renderTagEditorAliases();
        clearTagEditorError();
      }
    });

    tagEditorParentDisplay.addEventListener(
      "click",
      openTagEditorParentSearch,
    );
    tagEditorParentClear.addEventListener("click", clearTagEditorParent);
    tagEditorParentSuggestWidget = createTagSearch({
      input: tagEditorParentInput,
      list: tagEditorParentSuggest,
      idPrefix: "tag-editor-parent",
      filterItem: (item) => !isTagEditorSelfOrDescendant(item.tagPath),
      onChoose: chooseTagEditorParent,
      onAdd: chooseTagEditorParent,
      onError: showTagEditorError,
    });

    tagEditor.addEventListener("cancel", (event) => {
      event.preventDefault();
      closeTagEditor();
    });
    tagEditor.addEventListener("click", (event) => {
      if (event.target === tagEditor) {
        closeTagEditor();
      }
    });

    document.addEventListener("keydown", (event) => {
      if (event.key !== "Escape" || event.defaultPrevented) {
        return;
      }
      if (tagEditorOpen) {
        event.preventDefault();
        closeTagEditor();
        return;
      }
      if (
        ImageLightbox.isOpen() ||
        selectedImageItems.size > 0 ||
        selectionDrag.active
      ) {
        return;
      }
      if (tagDrawerOpen) {
        event.preventDefault();
        setTagDrawerOpen(false);
      }
    });
  }

  // ---------- 批量打标工作台 ----------

  function initWorkbenchCards() {
    for (const card of workbench.cards) {
      const root = workbenchPanel.querySelector(
        `.workbench-card[data-card="${card.index}"]`,
      );
      if (!root) {
        continue;
      }
      card.elements = {
        root,
        count: root.querySelector(".workbench-card-count"),
        search: root.querySelector(".workbench-card-search"),
        input: document.getElementById(`workbench-tag-${card.index}`),
        list: document.getElementById(`workbench-suggest-${card.index}`),
        stage: document.getElementById(`workbench-stage-${card.index}`),
        error: document.getElementById(`workbench-error-${card.index}`),
        add: root.querySelector(".workbench-card-add"),
      };
    }
  }

  function workbenchHasState() {
    return workbench.cards.some(
      (card) =>
        card.images.size > 0 ||
        card.additions.length > 0 ||
        card.removals.length > 0,
    );
  }

  function workbenchHasStagedTags() {
    return workbench.cards.some(
      (card) => card.additions.length > 0 || card.removals.length > 0,
    );
  }

  function setWorkbenchCardError(card, text) {
    card.error = String(text || "");
    if (card.elements && card.elements.error) {
      card.elements.error.textContent = card.error;
      card.elements.error.hidden = card.error === "";
    }
  }

  function syncWorkbenchDotSize() {
    if (!workbenchDotSize || !workbenchDotSizeValue) {
      return;
    }
    const min = Number(workbenchDotSize.min) || 6;
    const max = Number(workbenchDotSize.max) || 24;
    const raw = Number(workbenchDotSize.value);
    const size = Math.max(min, Math.min(max, Number.isFinite(raw) ? raw : 10));
    document.body.style.setProperty("--workbench-dot-size", `${size}px`);
    workbenchDotSizeValue.textContent = `${size}px`;
  }

  function updateWorkbenchUi() {
    const hasStaged = workbenchHasStagedTags();
    workbenchApply.disabled = workbench.applying || !hasStaged;
    workbenchApply.textContent = workbench.applying ? "应用中…" : "应用";
    workbenchClose.disabled = workbench.applying || workbench.closing;
    workbenchHandle.setAttribute("aria-expanded", String(workbench.open));
    for (const card of workbench.cards) {
      if (!card.elements) {
        continue;
      }
      card.elements.count.textContent = `${card.images.size} 张`;
      card.elements.add.setAttribute(
        "aria-expanded",
        String(card.searchOpen),
      );
    }
  }

  function syncWorkbenchCard(card) {
    if (!card.elements) {
      return;
    }
    const fragment = document.createDocumentFragment();
    for (const tag of card.additions) {
      const chip = document.createElement("button");
      chip.type = "button";
      chip.className = "workbench-chip workbench-chip-add";
      chip.textContent = tag.name;
      chip.title = `${tag.path || tag.name}\n点击移出暂存区`;
      chip.addEventListener("click", () =>
        unstageWorkbenchTag(card, tag.id),
      );
      fragment.appendChild(chip);
    }
    for (const tag of card.removals) {
      const chip = document.createElement("button");
      chip.type = "button";
      chip.className = "workbench-chip workbench-chip-remove";
      chip.textContent = tag.name;
      chip.title = `${tag.path || tag.name}\n点击移出暂存区`;
      chip.addEventListener("click", () =>
        unstageWorkbenchTag(card, tag.id),
      );
      fragment.appendChild(chip);
    }
    card.elements.stage.replaceChildren(fragment);
    updateWorkbenchUi();
  }

  function syncWorkbenchImage(cardElement) {
    const imageId = cardElement.dataset.imageId || "";
    const thumb = cardElement.querySelector(".thumb");
    if (!thumb) {
      return;
    }
    const old = thumb.querySelector(".workbench-dots");
    if (old) {
      old.remove();
    }
    if (!workbench.open || !imageId) {
      return;
    }
    const members = workbench.cards.filter((card) =>
      card.images.has(imageId),
    );
    if (members.length === 0) {
      return;
    }
    const dots = document.createElement("div");
    dots.className = "workbench-dots";
    for (const card of members) {
      const dot = document.createElement("span");
      dot.className = `workbench-dot ${card.dotClass}`;
      dot.title = `卡片 ${card.index}`;
      dots.appendChild(dot);
    }
    thumb.appendChild(dots);
  }

  function syncAllWorkbenchImages() {
    for (const card of allCards) {
      syncWorkbenchImage(card);
    }
  }

  function syncWorkbenchImagesForId(imageId) {
    if (!imageId) {
      return;
    }
    for (const card of allCards) {
      if (card.dataset.imageId === imageId) {
        syncWorkbenchImage(card);
        break;
      }
    }
  }

  function toggleWorkbenchMembership(card, imageId) {
    if (!card || !imageId) {
      return;
    }
    if (card.images.has(imageId)) {
      card.images.delete(imageId);
    } else {
      card.images.add(imageId);
    }
    syncWorkbenchCard(card);
    syncWorkbenchImagesForId(imageId);
  }

  function addWorkbenchMembership(card, imageId) {
    if (!card || !imageId || card.images.has(imageId)) {
      return;
    }
    card.images.add(imageId);
    syncWorkbenchCard(card);
    syncWorkbenchImagesForId(imageId);
  }

  function stageWorkbenchTag(card, tag, kind) {
    if (!tag || !tag.id) {
      return false;
    }
    const target = kind === "remove" ? card.removals : card.additions;
    const other = kind === "remove" ? card.additions : card.removals;
    const otherIndex = other.findIndex((item) => item.id === tag.id);
    if (otherIndex >= 0) {
      other.splice(otherIndex, 1);
    }
    if (!target.some((item) => item.id === tag.id)) {
      target.push({
        id: tag.id,
        name: tag.name,
        path: tag.path || tag.name,
      });
    }
    setWorkbenchCardError(card, "");
    syncWorkbenchCard(card);
    return true;
  }

  function unstageWorkbenchTag(card, tagId) {
    let changed = false;
    for (const list of [card.additions, card.removals]) {
      const index = list.findIndex((item) => item.id === tagId);
      if (index >= 0) {
        list.splice(index, 1);
        changed = true;
      }
    }
    if (changed) {
      syncWorkbenchCard(card);
    }
  }

  function closeWorkbenchSearch(card) {
    if (!card.searchOpen) {
      return;
    }
    card.searchOpen = false;
    if (card.elements) {
      card.elements.search.hidden = true;
      card.elements.input.value = "";
    }
    if (card.widget) {
      card.widget.clearChosen();
      card.widget.close();
    }
    syncWorkbenchCard(card);
  }

  function closeAllWorkbenchSearches(except) {
    for (const card of workbench.cards) {
      if (card !== except) {
        closeWorkbenchSearch(card);
      }
    }
  }

  function ensureWorkbenchWidget(card) {
    if (card.widget) {
      return card.widget;
    }
    card.widget = createTagSearch({
      input: card.elements.input,
      list: card.elements.list,
      idPrefix: `workbench-card-${card.index}`,
      onAdd: (tag) => stageWorkbenchTag(card, tag, "add"),
      onRemove: (tag) => stageWorkbenchTag(card, tag, "remove"),
      onError: (message) => setWorkbenchCardError(card, message),
    });
    return card.widget;
  }

  function toggleWorkbenchSearch(card) {
    if (workbench.applying) {
      return;
    }
    if (card.searchOpen) {
      closeWorkbenchSearch(card);
      return;
    }
    closeAllWorkbenchSearches(card);
    ensureWorkbenchWidget(card);
    card.searchOpen = true;
    card.elements.search.hidden = false;
    setWorkbenchCardError(card, "");
    syncWorkbenchCard(card);
    card.elements.input.focus();
    card.elements.input.click();
  }

  function openWorkbench() {
    if (workbench.open) {
      return;
    }
    clearSelection();
    workbench.open = true;
    document.body.classList.add("workbench-open");
    workbenchPanel.removeAttribute("inert");
    workbenchStatus.textContent = "";
    workbenchStatus.hidden = true;
    workbenchStatus.classList.remove("error");
    for (const card of workbench.cards) {
      setWorkbenchCardError(card, "");
      syncWorkbenchCard(card);
    }
    syncAllWorkbenchImages();
    updateWorkbenchUi();
    if (layoutAuto) {
      syncLayoutSettings();
    }
    if (window.innerWidth >= 900) {
      window.setTimeout(relayout, 240);
    }
  }

  function discardWorkbenchState() {
    for (const card of workbench.cards) {
      card.images.clear();
      card.additions.length = 0;
      card.removals.length = 0;
      setWorkbenchCardError(card, "");
      closeWorkbenchSearch(card);
      syncWorkbenchCard(card);
    }
    workbench.open = false;
    workbench.applying = false;
    workbench.closing = false;
    document.body.classList.remove("workbench-open");
    workbenchPanel.setAttribute("inert", "");
    workbenchStatus.textContent = "";
    workbenchStatus.hidden = true;
    workbenchStatus.classList.remove("error");
    syncAllWorkbenchImages();
    updateWorkbenchUi();
    if (layoutAuto) {
      syncLayoutSettings();
    }
    if (window.innerWidth >= 900) {
      window.setTimeout(relayout, 240);
    }
  }

  async function closeWorkbench() {
    if (!workbench.open || workbench.applying || workbench.closing) {
      return;
    }
    if (!workbenchHasState()) {
      discardWorkbenchState();
      return;
    }
    if (
      !window.ConfirmDialog ||
      typeof window.ConfirmDialog.open !== "function"
    ) {
      discardWorkbenchState();
      return;
    }
    workbench.closing = true;
    updateWorkbenchUi();
    let confirmed = false;
    try {
      confirmed = await window.ConfirmDialog.open({
        title: "关闭工作台",
        message: "关闭工作台将丢弃未应用的标签变更和图片归属，确定关闭吗？",
        confirmText: "关闭",
        cancelText: "取消",
        danger: true,
      });
    } finally {
      workbench.closing = false;
    }
    if (confirmed) {
      discardWorkbenchState();
    } else {
      updateWorkbenchUi();
    }
  }

  function workbenchCardForEvent(event) {
    if (event.button === 0 && (event.ctrlKey || event.metaKey)) {
      return workbench.cards[0];
    }
    if (event.button === 2 && (event.ctrlKey || event.metaKey)) {
      return workbench.cards[1];
    }
    if (event.button === 0 && event.altKey) {
      return workbench.cards[2];
    }
    if (event.button === 2 && event.altKey) {
      return workbench.cards[3];
    }
    return null;
  }

  function workbenchImageAtPoint(x, y, event) {
    let target = null;
    if (typeof document.elementFromPoint === "function") {
      target = document.elementFromPoint(x, y);
    }
    if (!target && event) {
      target = event.target;
    }
    if (!target || typeof target.closest !== "function") {
      return "";
    }
    const card = target.closest(".image-card");
    if (!card || !results.contains(card)) {
      return "";
    }
    return card.dataset.imageId || "";
  }

  function markWorkbenchClickSuppressed() {
    suppressSelectionClick = true;
    window.clearTimeout(suppressSelectionClickTimer);
    suppressSelectionClickTimer = window.setTimeout(() => {
      suppressSelectionClick = false;
    }, 0);
  }

  function paintWorkbenchAtPoint(x, y) {
    if (!workbenchDrag.active || !workbenchDrag.card) {
      return;
    }
    const imageId = workbenchImageAtPoint(x, y);
    if (imageId) {
      addWorkbenchMembership(workbenchDrag.card, imageId);
    }
  }

  function tickWorkbenchAutoScroll() {
    workbenchDrag.scrollFrame = 0;
    if (!workbenchDrag.active || !workbenchDrag.scrollSpeed) {
      return;
    }
    window.scrollBy(0, workbenchDrag.scrollSpeed);
    paintWorkbenchAtPoint(workbenchDrag.lastX, workbenchDrag.lastY);
    workbenchDrag.scrollFrame =
      window.requestAnimationFrame(tickWorkbenchAutoScroll);
  }

  function updateWorkbenchAutoScroll(y) {
    if (!workbenchDrag.active) {
      return;
    }
    const margin = 72;
    let speed = 0;
    if (y < margin) {
      speed = -Math.ceil((margin - y) / 4);
    } else if (y > window.innerHeight - margin) {
      speed = Math.ceil((y - (window.innerHeight - margin)) / 4);
    }
    speed = Math.max(-45, Math.min(45, speed));
    workbenchDrag.scrollSpeed = speed;
    if (speed && !workbenchDrag.scrollFrame) {
      workbenchDrag.scrollFrame =
        window.requestAnimationFrame(tickWorkbenchAutoScroll);
    } else if (!speed && workbenchDrag.scrollFrame) {
      window.cancelAnimationFrame(workbenchDrag.scrollFrame);
      workbenchDrag.scrollFrame = 0;
    }
  }

  function finishWorkbenchDrag(toggle) {
    if (!workbenchDrag.active) {
      return;
    }
    if (workbenchDrag.scrollFrame) {
      window.cancelAnimationFrame(workbenchDrag.scrollFrame);
    }
    const card = workbenchDrag.card;
    const imageId = workbenchDrag.imageId;
    const moved = workbenchDrag.moved;
    workbenchDrag.active = false;
    workbenchDrag.pointerId = null;
    workbenchDrag.card = null;
    workbenchDrag.imageId = "";
    workbenchDrag.moved = false;
    workbenchDrag.scrollSpeed = 0;
    workbenchDrag.scrollFrame = 0;
    document.body.classList.remove("workbench-dragging");
    if (toggle && !moved) {
      toggleWorkbenchMembership(card, imageId);
    }
    markWorkbenchClickSuppressed();
  }

  function beginWorkbenchDrag(event) {
    if (
      !workbench.open ||
      workbench.applying ||
      event.pointerType === "touch"
    ) {
      return;
    }
    const card = workbenchCardForEvent(event);
    if (!card) {
      return;
    }
    const imageId = workbenchImageAtPoint(
      event.clientX,
      event.clientY,
      event,
    );
    if (!imageId) {
      return;
    }
    event.preventDefault();
    finishWorkbenchDrag(false);
    workbenchDrag.active = true;
    workbenchDrag.pointerId =
      typeof event.pointerId === "number" ? event.pointerId : null;
    workbenchDrag.card = card;
    workbenchDrag.imageId = imageId;
    workbenchDrag.startX = event.clientX;
    workbenchDrag.startY = event.clientY;
    workbenchDrag.lastX = event.clientX;
    workbenchDrag.lastY = event.clientY;
    workbenchDrag.moved = false;
    exitHoverPreview();
  }

  function moveWorkbenchDrag(event) {
    if (
      !workbenchDrag.active ||
      (workbenchDrag.pointerId !== null &&
        event.pointerId !== workbenchDrag.pointerId)
    ) {
      return;
    }
    workbenchDrag.lastX = event.clientX;
    workbenchDrag.lastY = event.clientY;
    if (!workbenchDrag.moved) {
      const distance =
        Math.abs(event.clientX - workbenchDrag.startX) +
        Math.abs(event.clientY - workbenchDrag.startY);
      if (distance < 4) {
        return;
      }
      workbenchDrag.moved = true;
      document.body.classList.add("workbench-dragging");
      addWorkbenchMembership(workbenchDrag.card, workbenchDrag.imageId);
    }
    event.preventDefault();
    paintWorkbenchAtPoint(event.clientX, event.clientY);
    updateWorkbenchAutoScroll(event.clientY);
  }

  function endWorkbenchDrag(event) {
    if (
      !workbenchDrag.active ||
      (workbenchDrag.pointerId !== null &&
        event.pointerId !== workbenchDrag.pointerId)
    ) {
      return;
    }
    finishWorkbenchDrag(true);
  }

  function cancelWorkbenchDrag(event) {
    if (
      !workbenchDrag.active ||
      (workbenchDrag.pointerId !== null &&
        event.pointerId !== workbenchDrag.pointerId)
    ) {
      return;
    }
    finishWorkbenchDrag(false);
    markWorkbenchClickSuppressed();
  }

  async function applyWorkbench() {
    if (workbench.applying || !workbenchHasStagedTags()) {
      return;
    }
    workbench.applying = true;
    workbenchStatus.textContent = "";
    workbenchStatus.hidden = true;
    workbenchStatus.classList.remove("error");
    for (const card of workbench.cards) {
      setWorkbenchCardError(card, "");
    }
    updateWorkbenchUi();

    const summary = [];
    let failed = false;
    for (const card of workbench.cards) {
      if (
        card.images.size === 0 ||
        (card.additions.length === 0 && card.removals.length === 0)
      ) {
        continue;
      }
      const imageIds = [...card.images];
      const additions = card.additions.map((tag) => ({ ...tag }));
      const removeIds = card.removals.map((tag) => tag.id);
      try {
        await requestGraphQl(UPDATE_IMAGES_MUTATION, {
          input: {
            imageIds,
            addTagIds: additions.map((tag) => tag.id),
            removeTagIds: removeIds,
          },
        });
        applySelectionTagChangesLocally(imageIds, additions, removeIds);
        card.images.clear();
        card.additions.length = 0;
        card.removals.length = 0;
        setWorkbenchCardError(card, "");
        syncWorkbenchCard(card);
        summary.push(`卡片 ${card.index} 成功（${imageIds.length} 张）`);
      } catch (error) {
        failed = true;
        const message =
          error && error.message ? error.message : "应用失败";
        setWorkbenchCardError(card, message);
        syncWorkbenchCard(card);
        summary.push(`卡片 ${card.index} 失败：${message}`);
      }
    }

    workbench.applying = false;
    if (includeTags.length > 0 || excludeTags.length > 0) {
      search(true);
    }
    syncAllWorkbenchImages();
    updateWorkbenchUi();
    if (summary.length > 0) {
      workbenchStatus.textContent = `${summary.join("；")}。`;
      workbenchStatus.hidden = false;
      workbenchStatus.classList.toggle("error", failed);
    }
  }

  function suppressWorkbenchContextMenu(event) {
    if (!workbench.open) {
      return;
    }
    const target = event.target;
    if (!target || typeof target.closest !== "function") {
      return;
    }
    const card = target.closest(".image-card");
    if (card && results.contains(card)) {
      event.preventDefault();
    }
  }

  function bindWorkbenchUi() {
    initWorkbenchCards();
    workbenchHandle.addEventListener("click", () => {
      if (workbench.open) {
        closeWorkbench();
      } else {
        openWorkbench();
      }
    });
    workbenchClose.addEventListener("click", () => closeWorkbench());
    workbenchApply.addEventListener("click", () => applyWorkbench());
    if (workbenchDotSize) {
      workbenchDotSize.addEventListener("input", syncWorkbenchDotSize);
    }
    syncWorkbenchDotSize();

    for (const card of workbench.cards) {
      if (!card.elements) {
        continue;
      }
      card.elements.add.addEventListener("click", () =>
        toggleWorkbenchSearch(card),
      );
    }

    results.addEventListener("pointerdown", beginWorkbenchDrag);
    results.addEventListener("contextmenu", suppressWorkbenchContextMenu);
    document.addEventListener("pointermove", moveWorkbenchDrag, {
      passive: false,
    });
    document.addEventListener("pointerup", endWorkbenchDrag);
    document.addEventListener("pointercancel", cancelWorkbenchDrag);
    window.addEventListener("blur", () => finishWorkbenchDrag(false));
    updateWorkbenchUi();
  }

  // ---------- 事件绑定与启动 ----------

  const tagSuggestWidget = createTagSearch({
    input: tagInput,
    list: tagSuggest,
    idPrefix: "image-tag",
    onAdd: (tag) => {
      addStageTag(tag, "include");
      return true;
    },
    onRemove: (tag) => {
      addStageTag(tag, "exclude");
      return true;
    },
    onError: (message) => setStatus(message, "error"),
  });
  bindTagTreeUi();
  bindSelectionUi();
  bindWorkbenchUi();
  updateTagEditButton();

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
      if (!multiSelectEnabled() && selectedImageItems.size > 0) {
        clearSelection();
      }
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
