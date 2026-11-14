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

  const statusEl = document.getElementById("status");
  const artworkEl = document.getElementById("artwork");
  const titleEl = document.getElementById("detail-title");
  const metaEl = document.getElementById("detail-meta");
  const descriptionEl = document.getElementById("detail-description");
  const imagesSection = document.getElementById("images-section");
  const galleryEl = document.getElementById("gallery");
  const imageCountEl = document.getElementById("image-count");
  const tagsSection = document.getElementById("tags-section");
  const tagsEl = document.getElementById("detail-tags");
  const artistsSection = document.getElementById("artists-section");
  const artistsEl = document.getElementById("detail-artists");

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

  function renderTags(tags, paths) {
    const pathById = new Map();
    for (const item of Array.isArray(paths) ? paths : []) {
      pathById.set(item.id, item.path);
    }

    const fragment = document.createDocumentFragment();
    for (const tag of Array.isArray(tags) ? tags : []) {
      const path = pathById.get(tag.id);
      // 标签 chip 直接跳到列表页，并把该标签作为包含条件。
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

      const detail = [];
      const aliases = Array.isArray(tag.aliases) ? tag.aliases : [];
      if (aliases.length > 0) {
        detail.push(`别名：${aliases[0]}`);
      }
      if (path) {
        detail.push(`路径：${path}`);
      }
      if (tag.category) {
        detail.push(`分类：${tag.category}`);
      }
      detail.push("点击搜索该标签");
      chip.title = detail.join("\n");
      fragment.appendChild(chip);
    }
    tagsEl.replaceChildren(fragment);
    // replaceChildren 会把 fragment 里的节点移走，必须看容器本身。
    tagsSection.hidden = tagsEl.childElementCount === 0;
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

    renderTags(artwork.tags, artwork.tagPaths);
    renderArtists(artwork.artists);

    artworkEl.hidden = false;
  }

  async function load() {
    const params = new URLSearchParams(window.location.search);
    const artworkId = (params.get("id") || "").trim().toLowerCase();

    if (!UUID_PATTERN.test(artworkId)) {
      setStatus("链接里缺少有效的作品 id", "error");
      return;
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
      return;
    }

    if (Array.isArray(payload.errors) && payload.errors.length > 0) {
      const message = payload.errors[0].message || "未知错误";
      setStatus(`加载失败：${message}`, "error");
      return;
    }

    const artwork = payload.data && payload.data.artwork;
    if (!artwork) {
      setStatus("作品不存在或已被删除", "error");
      return;
    }

    setStatus("");
    render(artwork);
  }

  load();
})();
