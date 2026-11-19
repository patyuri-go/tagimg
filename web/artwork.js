(function () {
  "use strict";

  const UUID_PATTERN =
    /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

  // 详情页一次性取全量字段；tagPaths 用来给标签 chip 提供路径提示。
  const DETAIL_QUERY = `
    query ArtworkDetail($id: ID!) {
      artwork(id: $id) {
        id
        title
        description
        publishedAt
        createdAt
        updatedAt
        imageCount
        imageIds
        tagIds
        tags { id name category aliases }
        tagPaths { id path }
        artistIds
        artists { id name }
      }
    }
  `;

  const DELETE_MUTATION = `
    mutation DeleteArtwork($id: ID!) {
      deleteArtwork(id: $id) {
        deleted
        newlyOrphanedImageCount
      }
    }
  `;

  const UPDATE_MUTATION = `
    mutation UpdateArtwork($id: ID!, $input: UpdateArtworkInput!) {
      updateArtwork(id: $id, input: $input)
    }
  `;

  const RESCAN_MUTATION = `
    mutation RescanArtwork($artworkId: ID!) {
      rescanArtwork(artworkId: $artworkId)
    }
  `;

  const statusEl = document.getElementById("status");
  const artworkEl = document.getElementById("artwork");
  const titleEl = document.getElementById("detail-title");
  const metaEl = document.getElementById("detail-meta");
  const editFields = document.getElementById("edit-fields");
  const editTitle = document.getElementById("edit-title");
  const editPublished = document.getElementById("edit-published");
  const descriptionEl = document.getElementById("detail-description");
  const imagesSection = document.getElementById("images-section");
  const galleryEl = document.getElementById("gallery");
  const imageCountEl = document.getElementById("image-count");
  const editGalleryHint = document.getElementById("edit-gallery-hint");
  const tagsSection = document.getElementById("tags-section");
  const tagsEl = document.getElementById("detail-tags");
  const editTagSearch = document.getElementById("edit-tag-search");
  const editTagInput = document.getElementById("edit-tag-input");
  const editTagSuggest = document.getElementById("edit-tag-suggest");
  const editTagHint = document.getElementById("edit-tag-hint");
  const artistsSection = document.getElementById("artists-section");
  const artistsEl = document.getElementById("detail-artists");
  const editButton = document.getElementById("edit-button");
  const rescanButton = document.getElementById("rescan-button");
  const deleteButton = document.getElementById("delete-button");
  const editSaveBar = document.getElementById("edit-save-bar");
  const editSaveButton = document.getElementById("edit-save");
  const lightboxEl = document.getElementById("image-lightbox");
  const lightboxImage = document.getElementById("lightbox-image");
  const lightboxCaption = document.getElementById("lightbox-caption");
  const lightboxPrev = document.getElementById("lightbox-prev");
  const lightboxNext = document.getElementById("lightbox-next");
  const lightboxClose = document.getElementById("lightbox-close");
  const lightboxStage = document.getElementById("lightbox-stage");
  const lightboxError = document.getElementById("lightbox-error");

  let currentArtworkId = "";
  let currentArtwork = null;
  let editing = false;
  let deleting = false;
  let rescanning = false;
  let rescanned = false;
  let saving = false;
  // 编辑模式下待删除的已有标签 ID。
  const pendingRemovals = new Set();
  // 编辑模式下新增的标签，元素为 { id, name, path }。
  const pendingAdditions = [];
  // 当前详情页图片顺序，元素为 { imageId }。
  let galleryEntries = [];
  // 正在拖动的图片 ID。
  let draggedImageId = "";
  // 选择排序模式：点击图片按顺序编号，点击空白退出并应用新顺序。
  let selectionMode = false;
  // 已编号的图片 ID，数组下标 + 1 就是角标上的序号。
  let selectionOrder = [];
  // 放大浏览弹窗中的图片顺序和当前下标。
  let lightboxEntries = [];
  let lightboxIndex = 0;
  // 灯箱先用画廊里已加载的 small 图占位，再换成 regular 大图。
  const lightboxRegularImages = new Map();
  let lightboxToken = 0;
  let lightboxPlaceholderSrc = "";

  const tagSuggestWidget = TagSearch.create({
    input: editTagInput,
    list: editTagSuggest,
    idPrefix: "edit-tag",
    onEnter: () => addEditTag(),
  });

  function setStatus(text, kind) {
    statusEl.textContent = text;
    statusEl.hidden = !text;
    statusEl.classList.toggle("error", kind === "error");
  }

  function formatDate(value) {
    if (!value) {
      return "";
    }
    // 服务端返回 UTC 秒级 RFC3339，只做展示，不转换时区。
    return value.replace("T", " ").replace("Z", "").slice(0, 16);
  }

  // RFC3339（UTC）转成 datetime-local 需要的字面值。
  function toDateTimeLocal(value) {
    if (!value) {
      return "";
    }
    return value.replace("Z", "").slice(0, 19);
  }

  // datetime-local 的值补上秒和 Z，得到服务端要求的 RFC3339。
  function toPublishedAt(value) {
    const text = value.trim();
    if (!text) {
      return null;
    }
    const withSeconds = text.length === 16 ? `${text}:00` : text;
    return `${withSeconds}Z`;
  }

  function metaItem(label, value, mono) {
    const item = document.createElement("span");
    const name = document.createElement("span");
    name.className = "meta-label";
    name.textContent = label;
    const text = document.createElement("span");
    text.textContent = value;
    if (mono) {
      text.className = "mono";
    }
    item.append(name, text);
    return item;
  }

  // 画廊只用图片 ID 定位；顺序按服务端返回的 imageIds。
  function galleryEntriesFromArtwork(artwork) {
    const imageIds = Array.isArray(artwork.imageIds) ? artwork.imageIds : [];
    return imageIds.map((imageId) => ({ imageId, missing: false }));
  }

  function imageStreamUrl(imageId, size) {
    const url = `/api/v1/image/${encodeURIComponent(imageId)}`;
    return size ? `${url}?size=${encodeURIComponent(size)}` : url;
  }

  function cachedRegularImage(imageId) {
    let image = lightboxRegularImages.get(imageId);
    if (!image) {
      image = new Image();
      image.decoding = "async";
      image.fetchPriority = "low";
      image.src = imageStreamUrl(imageId, "regular");
      lightboxRegularImages.set(imageId, image);
    }
    return image;
  }

  function createMissingGalleryItem(imageId) {
    const placeholder = document.createElement("div");
    placeholder.className = "gallery-item gallery-item-missing";
    placeholder.dataset.imageId = imageId;
    const box = document.createElement("span");
    box.className = "gallery-placeholder";
    box.setAttribute("aria-label", "无法预览的图片");
    placeholder.appendChild(box);
    return placeholder;
  }

  // 没有 entity_id 的图片会让流接口返回 404；这里保留位置并显示空占位符，
  // 这样既不会暴露实体 ID，也仍然可以继续拖动排序。
  function showGalleryPlaceholder(entry, link) {
    entry.missing = true;
    link.classList.add("gallery-item-missing");
    link.removeAttribute("href");
    link.removeAttribute("target");
    const placeholder = document.createElement("span");
    placeholder.className = "gallery-placeholder";
    placeholder.setAttribute("aria-label", "无法预览的图片");
    link.replaceChildren(placeholder);
  }

  function createGalleryItem(entry, index, title) {
    const imageId = entry.imageId || "";
    const url = imageStreamUrl(imageId, "small");

    // 重新渲染时保留之前的加载失败状态，避免反复请求已知无法预览的图片。
    if (entry.missing) {
      return createMissingGalleryItem(imageId);
    }

    const link = document.createElement("a");
    link.className = "gallery-item";
    link.dataset.imageId = imageId;
    // 新标签页/中键打开时拿原图，网格内仍用小图。
    link.href = imageStreamUrl(imageId, "original");

    const image = document.createElement("img");
    image.src = url;
    image.alt = `${title || "作品图片"} 第 ${index + 1} 张`;
    image.loading = "lazy";
    image.decoding = "async";
    image.draggable = false;
    image.addEventListener("error", () => showGalleryPlaceholder(entry, link));

    // 只读视图点击图片打开放大浏览；编辑模式仍然进入选择排序。
    link.addEventListener("click", (event) => {
      if (link.classList.contains("gallery-item-missing")) {
        event.preventDefault();
        return;
      }
      if (editing) {
        event.preventDefault();
        return;
      }
      event.preventDefault();
      openLightbox(imageId);
    });

    link.appendChild(image);
    return link;
  }

  function renderGallery() {
    const title = currentArtwork ? currentArtwork.title : "";
    const fragment = document.createDocumentFragment();
    galleryEntries.forEach((entry, index) => {
      fragment.appendChild(createGalleryItem(entry, index, title));
    });
    galleryEl.replaceChildren(fragment);
    imageCountEl.textContent = `共 ${galleryEntries.length} 张`;
    imagesSection.hidden = galleryEntries.length === 0;
    setGalleryEditing(editing);
  }

  function lightboxImageId() {
    return lightboxEntries[lightboxIndex] || "";
  }

  function renderLightbox() {
    const imageId = lightboxImageId();
    if (!imageId) {
      closeLightbox();
      return;
    }
    const token = (lightboxToken += 1);

    const title = currentArtwork ? currentArtwork.title : "";
    lightboxError.hidden = true;
    lightboxImage.hidden = false;
    lightboxImage.dataset.imageId = imageId;
    // 先用画廊里已经加载过的缩略图顶上，regular 到达后再替换。
    const placeholderSrc = imageStreamUrl(imageId, "small");
    lightboxPlaceholderSrc = placeholderSrc;
    lightboxImage.src = placeholderSrc;
    lightboxImage.alt = `${title || "作品图片"} 第 ${lightboxIndex + 1} 张`;
    lightboxCaption.textContent = `第 ${lightboxIndex + 1} / ${lightboxEntries.length} 张`;

    const loader = cachedRegularImage(imageId);
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

    // 只有一张图片时不需要左右切换按钮。
    const multiple = lightboxEntries.length > 1;
    lightboxPrev.hidden = !multiple;
    lightboxNext.hidden = !multiple;
  }

  function openLightbox(imageId) {
    const available = galleryEntries.filter(
      (entry) => entry.imageId && !entry.missing,
    );
    const index = available.findIndex((entry) => entry.imageId === imageId);
    if (index < 0) {
      return;
    }

    lightboxEntries = available.map((entry) => entry.imageId);
    lightboxIndex = index;
    renderLightbox();
    if (!lightboxEl.open) {
      lightboxEl.showModal();
      document.body.classList.add("lightbox-open");
    }
    lightboxClose.focus();
  }

  function moveLightbox(step) {
    if (lightboxEntries.length === 0) {
      return;
    }
    lightboxIndex =
      (lightboxIndex + step + lightboxEntries.length) % lightboxEntries.length;
    renderLightbox();
  }

  function closeLightbox() {
    if (lightboxEl.open) {
      lightboxEl.close();
    }
  }

  // 放大浏览时才发现图片无法加载，也把画廊项换成占位符。
  function markImageMissing(imageId) {
    const entry = galleryEntries.find((item) => item.imageId === imageId);
    if (!entry || entry.missing) {
      return;
    }
    for (const link of galleryEl.querySelectorAll(".gallery-item")) {
      if (link.dataset.imageId === imageId) {
        showGalleryPlaceholder(entry, link);
        break;
      }
    }
  }

  // 把点击顺序编号显示在图片左上角；未编号的图片不显示角标。
  function applySelectionBadges() {
    for (const item of galleryEl.querySelectorAll(".gallery-item")) {
      const imageId = item.dataset.imageId || "";
      const order = selectionOrder.indexOf(imageId);
      let badge = item.querySelector(".gallery-order");
      if (order >= 0) {
        if (!badge) {
          badge = document.createElement("span");
          badge.className = "gallery-order";
          item.appendChild(badge);
        }
        badge.textContent = String(order + 1);
        badge.title = `第 ${order + 1} 张`;
        item.classList.add("gallery-item-numbered");
      } else {
        if (badge) {
          badge.remove();
        }
        item.classList.remove("gallery-item-numbered");
      }
    }
  }

  // 进入选择排序模式；已经编号的图片再点一次不会重复编号。
  function selectGalleryItem(imageId) {
    if (!editing || !imageId) {
      return;
    }
    if (!selectionMode) {
      selectionMode = true;
      selectionOrder = [];
    }
    if (selectionOrder.includes(imageId)) {
      return;
    }
    selectionOrder.push(imageId);
    applySelectionBadges();
    setGalleryEditing(editing);
  }

  // 退出选择排序：已编号图片按点击顺序排在前面，其余保持原相对顺序。
  function exitSelectionMode() {
    if (!selectionMode) {
      return;
    }
    const remaining = new Map(
      galleryEntries.map((entry) => [entry.imageId, entry]),
    );
    const ordered = [];
    for (const imageId of selectionOrder) {
      const entry = remaining.get(imageId);
      if (entry) {
        ordered.push(entry);
        remaining.delete(imageId);
      }
    }
    for (const entry of galleryEntries) {
      if (remaining.has(entry.imageId)) {
        ordered.push(entry);
        remaining.delete(entry.imageId);
      }
    }
    galleryEntries = ordered;
    selectionMode = false;
    selectionOrder = [];
    renderGallery();
  }

  function updateGalleryHint() {
    if (!editGalleryHint) {
      return;
    }
    const visible = editing && galleryEntries.length >= 2;
    editGalleryHint.hidden = !visible;
    editGalleryHint.textContent = selectionMode
      ? "选择排序中：按点击顺序编号，点击空白完成排序。"
      : "拖动图片到另一张图片上可交换位置；点击图片进入选择排序，按点击顺序编号，点击空白完成。";
  }

  function setGalleryEditing(enabled) {
    galleryEl.classList.toggle("gallery-edit", enabled);
    galleryEl.classList.toggle("gallery-selecting", enabled && selectionMode);
    const canDrag = enabled && !selectionMode;
    for (const item of galleryEl.querySelectorAll(".gallery-item")) {
      item.draggable = canDrag;
      item.classList.toggle("gallery-item-draggable", canDrag);
      if (canDrag) {
        item.setAttribute("aria-grabbed", "false");
      } else {
        item.removeAttribute("aria-grabbed");
      }
    }
    updateGalleryHint();
  }

  function clearGalleryDragStyles() {
    for (const item of galleryEl.querySelectorAll(".gallery-item")) {
      item.classList.remove("gallery-item-dragging", "gallery-item-drop");
      if (editing) {
        item.setAttribute("aria-grabbed", "false");
      }
    }
  }

  function tagPathMap(paths) {
    const pathById = new Map();
    for (const item of Array.isArray(paths) ? paths : []) {
      pathById.set(item.id, item.path);
    }
    return pathById;
  }

  function tagTitleLines(tag, path) {
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
    return lines;
  }

  // 只读模式下标签是跳转链接；编辑模式下点击切换删除状态。
  function createReadonlyTag(tag, path) {
    const chip = document.createElement("a");
    chip.className = "chip chip-link";
    chip.textContent = tag.name;

    const params = new URLSearchParams();
    params.set("tag", tag.id);
    params.set("name", tag.name);
    if (path) {
      params.set("path", path);
    }
    chip.href = `/?${params.toString()}`;

    const lines = tagTitleLines(tag, path);
    lines.push("点击搜索该标签");
    chip.title = lines.join("\n");
    return chip;
  }

  function createEditableTag(tag, path) {
    const removed = pendingRemovals.has(tag.id);
    const chip = document.createElement("button");
    chip.type = "button";
    chip.className = "chip chip-editable";
    if (removed) {
      chip.classList.add("chip-remove");
    }
    chip.textContent = tag.name;
    chip.setAttribute("aria-pressed", removed ? "true" : "false");

    const lines = tagTitleLines(tag, path);
    lines.push(removed ? "点击取消删除" : "点击标记删除");
    chip.title = lines.join("\n");

    chip.addEventListener("click", () => {
      if (pendingRemovals.has(tag.id)) {
        pendingRemovals.delete(tag.id);
      } else {
        pendingRemovals.add(tag.id);
      }
      renderTags();
    });
    return chip;
  }

  function createPendingTag(tag) {
    const chip = document.createElement("button");
    chip.type = "button";
    chip.className = "chip chip-editable chip-add";
    chip.textContent = tag.name;
    chip.title = `${tag.path}\n点击移除`;
    chip.addEventListener("click", () => {
      const index = pendingAdditions.findIndex((item) => item.id === tag.id);
      if (index >= 0) {
        pendingAdditions.splice(index, 1);
        renderTags();
      }
    });
    return chip;
  }

  function renderTags() {
    const tags =
      currentArtwork && Array.isArray(currentArtwork.tags)
        ? currentArtwork.tags
        : [];
    const pathById = tagPathMap(currentArtwork && currentArtwork.tagPaths);

    const fragment = document.createDocumentFragment();
    for (const tag of tags) {
      const path = pathById.get(tag.id);
      fragment.appendChild(
        editing
          ? createEditableTag(tag, path)
          : createReadonlyTag(tag, path),
      );
    }
    if (editing) {
      for (const tag of pendingAdditions) {
        fragment.appendChild(createPendingTag(tag));
      }
    }

    tagsEl.replaceChildren(fragment);
    // 编辑模式即使没有标签也要显示区块，才能放下方的搜索框。
    tagsSection.hidden = !editing && tags.length === 0;
  }

  function renderArtists(artists) {
    const fragment = document.createDocumentFragment();
    for (const artist of Array.isArray(artists) ? artists : []) {
      const chip = document.createElement("span");
      chip.className = "chip";
      chip.textContent = artist.name;
      fragment.appendChild(chip);
    }
    artistsEl.replaceChildren(fragment);
    artistsSection.hidden = artistsEl.childElementCount === 0;
  }

  function render(artwork) {
    closeLightbox();
    currentArtwork = artwork;
    const title = artwork.title || "（无标题）";
    titleEl.textContent = title;
    titleEl.title = artwork.title || "";
    document.title = `${title} · TagImg`;

    const meta = document.createDocumentFragment();
    meta.appendChild(metaItem("创建", formatDate(artwork.createdAt)));
    if (artwork.publishedAt) {
      meta.appendChild(metaItem("发布", formatDate(artwork.publishedAt)));
    }
    meta.appendChild(
      metaItem("图片", `${Number(artwork.imageCount) || 0} 张`),
    );
    meta.appendChild(metaItem("ID", artwork.id, true));
    metaEl.replaceChildren(meta);

    if (artwork.description) {
      descriptionEl.textContent = artwork.description;
      descriptionEl.hidden = false;
    } else {
      descriptionEl.hidden = true;
    }

    galleryEntries = galleryEntriesFromArtwork(artwork);
    renderGallery();

    renderTags();
    renderArtists(artwork.artists);

    artworkEl.hidden = false;
  }

  function setEditing(enabled) {
    closeLightbox();
    editing = enabled;
    editButton.textContent = enabled ? "退出编辑" : "编辑";
    editButton.setAttribute("aria-pressed", String(enabled));
    // 重扫与删除都属于编辑模式里的操作，退出编辑时一并收起。
    rescanButton.hidden = !enabled;
    deleteButton.hidden = !enabled;

    // 进入或退出编辑都回到服务端当前顺序，未保存的拖动不会留在只读视图。
    selectionMode = false;
    selectionOrder = [];
    galleryEntries = currentArtwork
      ? galleryEntriesFromArtwork(currentArtwork)
      : [];
    renderGallery();

    if (enabled) {
      pendingRemovals.clear();
      pendingAdditions.length = 0;
      editTitle.value = currentArtwork ? currentArtwork.title || "" : "";
      editPublished.value = currentArtwork
        ? toDateTimeLocal(currentArtwork.publishedAt)
        : "";
      titleEl.hidden = true;
      metaEl.hidden = true;
      editFields.hidden = false;
      editTagSearch.hidden = false;
      editTagHint.hidden = false;
      editSaveBar.hidden = false;
      editTagInput.value = "";
      tagSuggestWidget.clearChosen();
      tagSuggestWidget.close();
      setStatus("");
      renderTags();
    } else {
      titleEl.hidden = false;
      metaEl.hidden = false;
      editFields.hidden = true;
      editTagSearch.hidden = true;
      editTagHint.hidden = true;
      editSaveBar.hidden = true;
      editTagInput.value = "";
      tagSuggestWidget.clearChosen();
      tagSuggestWidget.close();
      pendingRemovals.clear();
      pendingAdditions.length = 0;
      renderTags();
    }
  }

  // 回车：把输入框里的完整路径加入新增列表，或取消已有标签的删除标记。
  async function addEditTag() {
    const normalized = TagSearch.trimPathSuffix(editTagInput.value);
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

    const existing =
      currentArtwork && Array.isArray(currentArtwork.tags)
        ? currentArtwork.tags.find((item) => item.id === tag.id)
        : null;
    if (existing) {
      // 重新添加一个已标记删除的标签等于取消删除。
      pendingRemovals.delete(tag.id);
    } else if (!pendingAdditions.some((item) => item.id === tag.id)) {
      pendingAdditions.push({ id: tag.id, name: tag.name, path: tag.path });
    }

    setStatus("");
    renderTags();
    // 保留完整路径并补上 ::，方便继续在它下面补全。
    editTagInput.value = `${tag.path}::`;
    tagSuggestWidget.clearChosen();
    tagSuggestWidget.close();
    editTagInput.focus();
    editTagInput.setSelectionRange(
      editTagInput.value.length,
      editTagInput.value.length,
    );
  }

  async function saveEdits() {
    if (saving || !currentArtworkId || !currentArtwork) {
      return;
    }

    const title = editTitle.value.trim();
    if (!title) {
      setStatus("标题不能为空", "error");
      editTitle.focus();
      return;
    }

    const input = { title };
    const publishedAt = toPublishedAt(editPublished.value);
    if (publishedAt) {
      input.publishedAt = publishedAt;
    } else if (currentArtwork.publishedAt) {
      input.clearPublishedAt = true;
    }
    if (pendingAdditions.length > 0) {
      input.addTagIds = pendingAdditions.map((tag) => tag.id);
    }
    if (pendingRemovals.size > 0) {
      input.removeTagIds = Array.from(pendingRemovals);
    }

    // 只在真正拖动过时提交完整顺序；后端会统一重写 position 0..n-1。
    const imageIds = galleryEntries.map((entry) => entry.imageId);
    const originalImageIds = Array.isArray(currentArtwork.imageIds)
      ? currentArtwork.imageIds
      : [];
    const orderChanged =
      imageIds.length !== originalImageIds.length ||
      imageIds.some((imageId, index) => imageId !== originalImageIds[index]);
    if (orderChanged) {
      input.imageIds = imageIds;
    }

    saving = true;
    editSaveButton.disabled = true;
    editSaveButton.textContent = "保存中…";
    setStatus("保存中…");

    let payload = null;
    let failure = null;
    try {
      const response = await fetch("/graphql", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          query: UPDATE_MUTATION,
          variables: { id: currentArtworkId, input },
        }),
      });
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
        failure = new Error(message);
      } else if (
        payload &&
        Array.isArray(payload.errors) &&
        payload.errors.length > 0
      ) {
        failure = new Error(payload.errors[0].message || "未知错误");
      } else if (!payload || !payload.data || !payload.data.updateArtwork) {
        failure = new Error("响应缺少 updateArtwork 数据");
      }
    } catch (error) {
      failure = error;
    }

    if (failure) {
      saving = false;
      editSaveButton.disabled = false;
      editSaveButton.textContent = "保存";
      setStatus(`保存失败：${failure.message}`, "error");
      return;
    }

    // 保存后退出编辑并重新拉取，保证标签路径、更新时间等展示一致。
    setEditing(false);
    const loaded = await load();
    if (loaded) {
      setStatus("已保存");
    }

    saving = false;
    editSaveButton.disabled = false;
    editSaveButton.textContent = "保存";
  }

  function finishDelete(message, kind) {
    artworkEl.hidden = true;
    editButton.hidden = true;
    rescanButton.hidden = true;
    deleteButton.hidden = true;
    editSaveBar.hidden = true;
    setStatus(message, kind);
    // 留一点时间让用户看到游离图片数量，再回到列表。
    window.setTimeout(() => {
      window.location.assign("/");
    }, 1200);
  }

  async function rescanArtworkImages() {
    if (rescanning || rescanned || deleting || saving || !currentArtworkId) {
      return;
    }

    const count = currentArtwork ? currentArtwork.imageIds.length : 0;
    if (
      !window.confirm(
        `确定重新扫描这个作品的 ${count} 张图片吗？\n` +
          "会重新下载并打标每一张；全部成功后才替换向量并补充标签，失败时保持原样。\n" +
          "扫描期间其它扫描/添加任务会排队。",
      )
    ) {
      return;
    }

    rescanning = true;
    rescanButton.disabled = true;
    setStatus("重扫任务提交中…");

    let payload;
    try {
      const response = await fetch("/graphql", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          query: RESCAN_MUTATION,
          variables: { artworkId: currentArtworkId },
        }),
      });
      if (!response.ok) {
        throw new Error(`HTTP ${response.status}`);
      }
      payload = await response.json();
    } catch (error) {
      rescanning = false;
      rescanButton.disabled = false;
      setStatus(`重扫提交失败：${error.message}`, "error");
      return;
    }

    if (Array.isArray(payload.errors) && payload.errors.length > 0) {
      rescanning = false;
      rescanButton.disabled = false;
      const message = payload.errors[0].message || "未知错误";
      setStatus(`重扫提交失败：${message}`, "error");
      return;
    }

    rescanning = false;
    rescanned = true;
    // 没有完成信号：按钮保持置灰，刷新页面才能再提交，避免重复排队。
    rescanButton.textContent = "重扫已提交";
    rescanButton.disabled = true;
    setStatus("重扫已提交，完成后刷新页面查看结果");
  }

  async function deleteCurrentArtwork() {
    if (deleting || saving || !currentArtworkId) {
      return;
    }
    if (
      !window.confirm(
        "确定删除这个作品吗？\n作品会被永久删除，只属于它的图片会变成游离图片。",
      )
    ) {
      return;
    }

    deleting = true;
    editButton.disabled = true;
    deleteButton.disabled = true;
    setStatus("删除中…");

    let payload;
    try {
      const response = await fetch("/graphql", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          query: DELETE_MUTATION,
          variables: { id: currentArtworkId },
        }),
      });
      if (!response.ok) {
        throw new Error(`HTTP ${response.status}`);
      }
      payload = await response.json();
    } catch (error) {
      deleting = false;
      editButton.disabled = false;
      deleteButton.disabled = false;
      setStatus(`删除失败：${error.message}`, "error");
      return;
    }

    if (Array.isArray(payload.errors) && payload.errors.length > 0) {
      deleting = false;
      editButton.disabled = false;
      deleteButton.disabled = false;
      const message = payload.errors[0].message || "未知错误";
      setStatus(`删除失败：${message}`, "error");
      return;
    }

    const result = payload.data && payload.data.deleteArtwork;
    if (!result || !result.deleted) {
      // 作品已被其他人删掉：按已删除处理，避免按钮停在禁用状态。
      finishDelete("作品不存在或已被删除", "error");
      return;
    }

    const orphaned = Number(result.newlyOrphanedImageCount) || 0;
    finishDelete(
      orphaned > 0
        ? `已删除作品，${orphaned} 张图片变为游离图片`
        : "已删除作品",
    );
  }

  function galleryItemFromEvent(event) {
    const target = event.target;
    return target && target.closest ? target.closest(".gallery-item") : null;
  }

  galleryEl.addEventListener("click", (event) => {
    if (!editing) {
      return;
    }
    const item = galleryItemFromEvent(event);
    if (!item) {
      return;
    }
    // 编辑模式下点击图片不打开原图，而是按点击顺序编号。
    event.preventDefault();
    selectGalleryItem(item.dataset.imageId || "");
  });

  // 点击图片之外的空白区域退出选择排序；capture 保证先应用顺序再处理保存等按钮。
  document.addEventListener(
    "click",
    (event) => {
      if (!selectionMode) {
        return;
      }
      const target = event.target;
      const item =
        target && target.closest ? target.closest(".gallery-item") : null;
      if (!item) {
        exitSelectionMode();
      }
    },
    true,
  );

  function galleryItemFromEvent(event) {
    const target = event.target;
    return target && target.closest ? target.closest(".gallery-item") : null;
  }

  galleryEl.addEventListener("dragstart", (event) => {
    if (!editing || selectionMode) {
      event.preventDefault();
      return;
    }
    const item = galleryItemFromEvent(event);
    if (!item || !galleryEl.contains(item)) {
      event.preventDefault();
      return;
    }
    draggedImageId = item.dataset.imageId || "";
    if (!draggedImageId) {
      event.preventDefault();
      return;
    }
    if (event.dataTransfer) {
      event.dataTransfer.effectAllowed = "move";
      event.dataTransfer.setData("text/plain", draggedImageId);
    }
    item.classList.add("gallery-item-dragging");
    item.setAttribute("aria-grabbed", "true");
  });

  galleryEl.addEventListener("dragend", () => {
    draggedImageId = "";
    clearGalleryDragStyles();
  });

  galleryEl.addEventListener("dragover", (event) => {
    if (!editing || selectionMode || !draggedImageId) {
      return;
    }
    const item = galleryItemFromEvent(event);
    // 只有拖到另一张图片上才允许放下，空白处不改变顺序。
    if (!item || item.dataset.imageId === draggedImageId) {
      return;
    }
    event.preventDefault();
    if (event.dataTransfer) {
      event.dataTransfer.dropEffect = "move";
    }
    item.classList.add("gallery-item-drop");
  });

  galleryEl.addEventListener("dragleave", (event) => {
    const item = galleryItemFromEvent(event);
    if (item) {
      item.classList.remove("gallery-item-drop");
    }
  });

  galleryEl.addEventListener("drop", (event) => {
    if (!editing || selectionMode || !draggedImageId) {
      return;
    }
    const item = galleryItemFromEvent(event);
    const targetImageId = item ? item.dataset.imageId || "" : "";
    const from = galleryEntries.findIndex(
      (entry) => entry.imageId === draggedImageId,
    );
    const to = galleryEntries.findIndex(
      (entry) => entry.imageId === targetImageId,
    );
    if (from < 0 || to < 0 || from === to) {
      draggedImageId = "";
      clearGalleryDragStyles();
      return;
    }
    event.preventDefault();

    // 拖到另一张图片上表示两张图片交换位置。
    const moved = galleryEntries[from];
    galleryEntries[from] = galleryEntries[to];
    galleryEntries[to] = moved;

    draggedImageId = "";
    renderGallery();
  });

  async function load() {
    const params = new URLSearchParams(window.location.search);
    const artworkId = (params.get("id") || "").trim().toLowerCase();

    if (!UUID_PATTERN.test(artworkId)) {
      setStatus("链接里缺少有效的作品 id", "error");
      return false;
    }

    setStatus("加载中…");

    let payload;
    try {
      const response = await fetch("/graphql", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          query: DETAIL_QUERY,
          variables: { id: artworkId },
        }),
      });
      if (!response.ok) {
        throw new Error(`HTTP ${response.status}`);
      }
      payload = await response.json();
    } catch (error) {
      setStatus(`加载失败：${error.message}`, "error");
      return false;
    }

    if (Array.isArray(payload.errors) && payload.errors.length > 0) {
      const message = payload.errors[0].message || "未知错误";
      setStatus(`加载失败：${message}`, "error");
      return false;
    }

    const artwork = payload.data && payload.data.artwork;
    if (!artwork) {
      setStatus("作品不存在或已被删除", "error");
      return false;
    }

    setStatus("");
    render(artwork);
    currentArtworkId = artwork.id;
    editButton.hidden = false;
    return true;
  }

  editButton.addEventListener("click", () => {
    if (saving || deleting) {
      return;
    }
    setEditing(!editing);
  });
  rescanButton.addEventListener("click", rescanArtworkImages);
  deleteButton.addEventListener("click", deleteCurrentArtwork);
  editSaveButton.addEventListener("click", saveEdits);

  lightboxPrev.addEventListener("click", () => moveLightbox(-1));
  lightboxNext.addEventListener("click", () => moveLightbox(1));
  lightboxClose.addEventListener("click", closeLightbox);
  lightboxStage.addEventListener("click", (event) => {
    if (event.target === lightboxStage) {
      closeLightbox();
    }
  });
  lightboxEl.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      event.preventDefault();
      closeLightbox();
    } else if (event.key === "ArrowLeft") {
      event.preventDefault();
      moveLightbox(-1);
    } else if (event.key === "ArrowRight") {
      event.preventDefault();
      moveLightbox(1);
    }
  });
  lightboxEl.addEventListener("close", () => {
    document.body.classList.remove("lightbox-open");
    lightboxToken += 1;
    lightboxPlaceholderSrc = "";
    lightboxEntries = [];
    lightboxIndex = 0;
    lightboxImage.removeAttribute("src");
    lightboxImage.removeAttribute("data-image-id");
    lightboxImage.alt = "";
    lightboxImage.hidden = false;
    lightboxError.hidden = true;
  });
  lightboxImage.addEventListener("load", () => {
    lightboxImage.hidden = false;
    lightboxError.hidden = true;
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
    const imageId = lightboxImageId();
    if (imageId) {
      markImageMissing(imageId);
    }
    lightboxImage.hidden = true;
    lightboxError.hidden = false;
  });

  load();
})();
