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
        entityIds
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
  const tagsSection = document.getElementById("tags-section");
  const tagsEl = document.getElementById("detail-tags");
  const editTagSearch = document.getElementById("edit-tag-search");
  const editTagInput = document.getElementById("edit-tag-input");
  const editTagSuggest = document.getElementById("edit-tag-suggest");
  const editTagHint = document.getElementById("edit-tag-hint");
  const artistsSection = document.getElementById("artists-section");
  const artistsEl = document.getElementById("detail-artists");
  const editButton = document.getElementById("edit-button");
  const deleteButton = document.getElementById("delete-button");
  const editSaveBar = document.getElementById("edit-save-bar");
  const editSaveButton = document.getElementById("edit-save");

  let currentArtworkId = "";
  let currentArtwork = null;
  let editing = false;
  let deleting = false;
  let saving = false;
  // 编辑模式下待删除的已有标签 ID。
  const pendingRemovals = new Set();
  // 编辑模式下新增的标签，元素为 { id, name, path }。
  const pendingAdditions = [];

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

  function createGalleryItem(entityId, index, title) {
    const link = document.createElement("a");
    link.className = "gallery-item";
    link.href = `/api/v1/image/${encodeURIComponent(entityId)}?download=1`;
    link.target = "_blank";
    link.rel = "noopener";

    const image = document.createElement("img");
    image.src = `/api/v1/image/${encodeURIComponent(entityId)}`;
    image.alt = `${title || "作品图片"} 第 ${index + 1} 张`;
    image.loading = "lazy";
    image.decoding = "async";
    image.addEventListener("error", () => {
      const placeholder = document.createElement("span");
      placeholder.className = "gallery-placeholder";
      placeholder.textContent = "图片加载失败";
      link.replaceChildren(placeholder);
    });

    link.appendChild(image);
    return link;
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

    const entityIds = Array.isArray(artwork.entityIds)
      ? artwork.entityIds
      : [];
    const gallery = document.createDocumentFragment();
    entityIds.forEach((entityId, index) => {
      gallery.appendChild(createGalleryItem(entityId, index, artwork.title));
    });
    galleryEl.replaceChildren(gallery);
    imageCountEl.textContent = `共 ${entityIds.length} 张`;
    imagesSection.hidden = entityIds.length === 0;

    renderTags();
    renderArtists(artwork.artists);

    artworkEl.hidden = false;
  }

  function setEditing(enabled) {
    editing = enabled;
    editButton.textContent = enabled ? "退出编辑" : "编辑";
    editButton.setAttribute("aria-pressed", String(enabled));
    // 删除属于编辑模式里的破坏性操作，退出编辑时一并收起。
    deleteButton.hidden = !enabled;

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
    deleteButton.hidden = true;
    editSaveBar.hidden = true;
    setStatus(message, kind);
    // 留一点时间让用户看到游离图片数量，再回到列表。
    window.setTimeout(() => {
      window.location.assign("/");
    }, 1200);
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
  deleteButton.addEventListener("click", deleteCurrentArtwork);
  editSaveButton.addEventListener("click", saveEdits);

  load();
})();
