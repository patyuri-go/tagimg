// 图片灯箱。无构建步骤，ES module，直接导出单例。
//
//   import { ImageLightbox } from "/lightbox.js";
//
//   ImageLightbox.init({
//     renderSidebar(entry, refs) {
//       // refs.similar / refs.artworks / refs.tags / refs.meta
//     },
//     onImageUnavailable(imageId) {},
//   });
//
//   ImageLightbox.open(entries, startIndex);
//
// entries 里每项至少包含 id，可选 alt；其余字段对模块不透明。
// 原图状态只属于当前这次打开：关闭后重新打开会回到预览图。

const ZOOM_MIN = 1;
const ZOOM_MAX = 8;

function imageStreamUrl(imageId, size) {
  const url = `/api/v1/image/${encodeURIComponent(imageId)}`;
  return size ? `${url}?size=${encodeURIComponent(size)}` : url;
}

let options = {
  renderSidebar: null,
  onImageUnavailable: null,
};

let dialog = null;
let captionEl = null;
let closeButton = null;
let stage = null;
let image = null;
let errorEl = null;
let loadingEl = null;
let originalButton = null;
let prevButton = null;
let nextButton = null;
let similarEl = null;
let artworksEl = null;
let tagsEl = null;
let metaEl = null;

let entries = [];
let index = -1;
let loadToken = 0;
let placeholderSrc = "";
// preview | original-loading | original | original-failed
let viewMode = "preview";
let originalLoading = false;
let previewPlaceholderFailed = false;
let previewRegularFailed = false;
let previewUnavailableReported = false;
const originalIds = new Set();
const regularImages = new Map();
let originalLoader = null;

const zoom = {
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

function ensureDom() {
  if (dialog) {
    return;
  }

  dialog = document.createElement("dialog");
  dialog.className = "dialog image-lightbox";
  dialog.setAttribute("aria-labelledby", "lightbox-caption");

  const head = document.createElement("header");
  head.className = "dialog-head image-lightbox-head";

  captionEl = document.createElement("h2");
  captionEl.id = "lightbox-caption";
  captionEl.className = "lightbox-caption";

  closeButton = document.createElement("button");
  closeButton.type = "button";
  closeButton.className = "icon-button lightbox-close";
  closeButton.setAttribute("aria-label", "关闭图片浏览");
  closeButton.textContent = "×";

  head.append(captionEl, closeButton);

  const body = document.createElement("div");
  body.className = "image-lightbox-body";

  stage = document.createElement("div");
  stage.className = "image-lightbox-stage";

  image = document.createElement("img");
  image.className = "lightbox-image";
  image.alt = "";

  errorEl = document.createElement("p");
  errorEl.className = "lightbox-error";
  errorEl.textContent = "图片无法加载";
  errorEl.hidden = true;

  loadingEl = document.createElement("p");
  loadingEl.className = "lightbox-loading";
  loadingEl.textContent = "正在加载原图…";
  loadingEl.hidden = true;

  originalButton = document.createElement("button");
  originalButton.type = "button";
  originalButton.className = "lightbox-original";
  originalButton.textContent = "原图";
  originalButton.hidden = true;

  prevButton = document.createElement("button");
  prevButton.type = "button";
  prevButton.className = "lightbox-nav lightbox-prev";
  prevButton.title = "上一张（←）";
  prevButton.setAttribute("aria-label", "上一张图片");
  prevButton.textContent = "‹";

  nextButton = document.createElement("button");
  nextButton.type = "button";
  nextButton.className = "lightbox-nav lightbox-next";
  nextButton.title = "下一张（→）";
  nextButton.setAttribute("aria-label", "下一张图片");
  nextButton.textContent = "›";

  stage.append(
    image,
    errorEl,
    loadingEl,
    originalButton,
    prevButton,
    nextButton,
  );

  const side = document.createElement("aside");
  side.className = "image-lightbox-side";

  similarEl = document.createElement("a");
  similarEl.className = "lightbox-similar-link";
  similarEl.href = "/images.html";
  similarEl.textContent = "查找相似图片";

  artworksEl = document.createElement("div");
  artworksEl.className = "image-lightbox-artworks";

  tagsEl = document.createElement("div");
  tagsEl.className = "image-lightbox-tags";

  metaEl = document.createElement("dl");
  metaEl.className = "image-meta";

  const artworksSection = document.createElement("section");
  artworksSection.className = "image-info-section";
  const artworksTitle = document.createElement("h3");
  artworksTitle.className = "image-info-title";
  artworksTitle.textContent = "所属作品";
  artworksSection.append(artworksTitle, artworksEl);

  const tagsSection = document.createElement("section");
  tagsSection.className = "image-info-section";
  const tagsTitle = document.createElement("h3");
  tagsTitle.className = "image-info-title";
  tagsTitle.textContent = "标签";
  tagsSection.append(tagsTitle, tagsEl);

  const metaSection = document.createElement("section");
  metaSection.className = "image-info-section";
  const metaTitle = document.createElement("h3");
  metaTitle.className = "image-info-title";
  metaTitle.textContent = "详情";
  metaSection.append(metaTitle, metaEl);

  side.append(similarEl, artworksSection, tagsSection, metaSection);
  body.append(stage, side);
  dialog.append(head, body);
  document.body.appendChild(dialog);

  bindEvents();
}

function bindEvents() {
  closeButton.addEventListener("click", () => dialog.close());
  prevButton.addEventListener("click", () => moveLightbox(-1));
  nextButton.addEventListener("click", () => moveLightbox(1));
  originalButton.addEventListener("click", requestOriginal);

  // 点击遮罩（dialog 自身而不是内容）关闭。
  dialog.addEventListener("click", (event) => {
    if (event.target === dialog) {
      dialog.close();
    }
  });

  // 点击图片周围的空白也关闭。
  stage.addEventListener("click", (event) => {
    if (event.target === stage) {
      dialog.close();
    }
  });

  stage.addEventListener(
    "wheel",
    (event) => {
      if (!dialog.open || event.deltaY === 0) {
        return;
      }
      event.preventDefault();
      zoomLightboxAt(
        event.clientX,
        event.clientY,
        Math.exp(-event.deltaY * 0.0015),
      );
    },
    { passive: false },
  );

  image.addEventListener("dragstart", (event) => event.preventDefault());
  image.addEventListener("load", () => {
    if (!image.getAttribute("src")) {
      return;
    }
    errorEl.hidden = true;
    image.hidden = false;
    if (zoom.scale > ZOOM_MIN + 0.001) {
      clampLightboxZoom();
      applyLightboxZoom();
    }
  });
  image.addEventListener("error", handleImageError);

  image.addEventListener("pointerdown", beginLightboxDrag);
  image.addEventListener("pointermove", moveLightboxDrag);
  image.addEventListener("pointerup", endLightboxDrag);
  image.addEventListener("pointercancel", endLightboxDrag);
  image.addEventListener("lostpointercapture", endLightboxDrag);

  dialog.addEventListener("close", cleanupLightbox);

  document.addEventListener("keydown", (event) => {
    if (!dialog.open) {
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
}

function currentEntry() {
  return index >= 0 && index < entries.length ? entries[index] : null;
}

function currentImageId() {
  const entry = currentEntry();
  return entry ? entry.id : "";
}

function renderLightbox() {
  const entry = currentEntry();
  if (!entry) {
    dialog.close();
    return;
  }

  const token = ++loadToken;
  cancelOriginalLoad();

  resetLightboxZoom();
  errorEl.textContent = "图片无法加载";
  errorEl.hidden = true;
  loadingEl.hidden = true;
  image.hidden = false;

  captionEl.textContent = `第 ${index + 1} / ${entries.length} 张`;
  prevButton.hidden = entries.length <= 1;
  nextButton.hidden = entries.length <= 1;

  image.alt = entry.alt || `图片 ${index + 1}`;
  image.dataset.imageId = entry.id;
  placeholderSrc = "";

  originalLoading = false;

  if (originalIds.has(entry.id)) {
    beginOriginal(token, entry.id);
  } else {
    beginPreview(token, entry.id);
  }

  renderSidebar(entry);
}

function beginPreview(token, imageId) {
  viewMode = "preview";
  previewPlaceholderFailed = false;
  previewRegularFailed = false;
  previewUnavailableReported = false;
  placeholderSrc = imageStreamUrl(imageId, "small");
  image.hidden = false;
  errorEl.hidden = true;
  loadingEl.hidden = true;
  updateOriginalButton();
  image.src = placeholderSrc;

  const loader = cachedRegularImage(imageId);
  const apply = () => {
    if (token !== loadToken) {
      return;
    }
    if (image.dataset.imageId !== imageId) {
      return;
    }
    if (!loader.naturalWidth) {
      return;
    }
    image.src = loader.currentSrc || loader.src;
  };
  const markRegularFailed = () => {
    if (token !== loadToken || image.dataset.imageId !== imageId) {
      return;
    }
    previewRegularFailed = true;
    maybeReportUnavailable();
  };

  if (loader.complete && loader.naturalWidth > 0) {
    apply();
  } else if (loader.complete) {
    markRegularFailed();
  } else {
    loader.addEventListener("load", apply, { once: true });
    loader.addEventListener("error", markRegularFailed, { once: true });
  }
}

function cachedRegularImage(imageId) {
  const cached = regularImages.get(imageId);
  if (cached) {
    return cached;
  }
  const loader = new Image();
  loader.decoding = "async";
  loader.fetchPriority = "low";
  loader.src = imageStreamUrl(imageId, "regular");
  regularImages.set(imageId, loader);
  return loader;
}

function updateOriginalButton() {
  if (!currentEntry()) {
    originalButton.hidden = true;
    return;
  }
  if (viewMode === "original") {
    originalButton.hidden = true;
    return;
  }

  originalButton.hidden = false;
  if (viewMode === "original-loading") {
    originalButton.disabled = true;
    originalButton.textContent = "加载中…";
  } else if (viewMode === "original-failed") {
    originalButton.disabled = false;
    originalButton.textContent = "重试原图";
  } else {
    originalButton.disabled = false;
    originalButton.textContent = "原图";
  }
}

function requestOriginal() {
  if (!dialog.open || originalLoading || viewMode === "original") {
    return;
  }
  const entry = currentEntry();
  if (!entry) {
    return;
  }
  beginOriginal(++loadToken, entry.id);
}

function beginOriginal(token, imageId) {
  cancelOriginalLoad();
  viewMode = "original-loading";
  originalLoading = true;
  image.removeAttribute("src");
  image.hidden = true;
  resetLightboxZoom();
  errorEl.hidden = true;
  loadingEl.hidden = false;
  loadingEl.textContent = "正在加载原图…";
  updateOriginalButton();

  const loader = new Image();
  originalLoader = loader;
  loader.decoding = "async";
  loader.src = imageStreamUrl(imageId, "original");

  const onLoad = () => {
    if (token !== loadToken || image.dataset.imageId !== imageId) {
      return;
    }
    originalLoader = null;
    viewMode = "original";
    originalLoading = false;
    originalIds.add(imageId);
    image.src = loader.currentSrc || loader.src;
    image.hidden = false;
    loadingEl.hidden = true;
    errorEl.hidden = true;
    updateOriginalButton();
  };

  const onError = () => {
    if (token !== loadToken || image.dataset.imageId !== imageId) {
      return;
    }
    originalLoader = null;
    failOriginal();
  };

  loader.addEventListener("load", onLoad, { once: true });
  loader.addEventListener("error", onError, { once: true });
}

function cancelOriginalLoad() {
  if (!originalLoader) {
    return;
  }
  const loader = originalLoader;
  originalLoader = null;
  loader.src = "";
}

function failOriginal() {
  const imageId = currentImageId();
  viewMode = "original-failed";
  originalLoading = false;
  if (imageId) {
    originalIds.delete(imageId);
  }
  image.hidden = true;
  loadingEl.hidden = true;
  errorEl.textContent = "原图加载失败";
  errorEl.hidden = false;
  updateOriginalButton();
}

function handleImageError() {
  if (viewMode === "original-loading" || viewMode === "original") {
    failOriginal();
    return;
  }

  const src = image.getAttribute("src");
  if (!src) {
    return;
  }
  if (src !== placeholderSrc) {
    // regular 失败时退回 small；small 也失败才认为图片不可用。
    previewRegularFailed = true;
    image.src = placeholderSrc;
    maybeReportUnavailable();
    return;
  }

  previewPlaceholderFailed = true;
  image.hidden = true;
  errorEl.textContent = "图片无法加载";
  errorEl.hidden = false;
  maybeReportUnavailable();
}

function maybeReportUnavailable() {
  if (previewUnavailableReported) {
    return;
  }
  if (!previewPlaceholderFailed || !previewRegularFailed) {
    return;
  }
  const entry = currentEntry();
  if (!entry) {
    return;
  }
  previewUnavailableReported = true;
  if (options.onImageUnavailable) {
    options.onImageUnavailable(entry.id);
  }
}

function moveLightbox(step) {
  if (entries.length <= 1) {
    return;
  }
  index = (index + step + entries.length) % entries.length;
  renderLightbox();
}

function renderSidebar(entry) {
  artworksEl.replaceChildren();
  tagsEl.replaceChildren();
  metaEl.replaceChildren();
  if (options.renderSidebar) {
    options.renderSidebar(entry, {
      similar: similarEl,
      artworks: artworksEl,
      tags: tagsEl,
      meta: metaEl,
    });
  }
}

function cleanupLightbox() {
  document.body.classList.remove("lightbox-open");
  loadToken += 1;
  cancelOriginalLoad();
  entries = [];
  index = -1;
  originalIds.clear();
  viewMode = "preview";
  originalLoading = false;
  previewPlaceholderFailed = false;
  previewRegularFailed = false;
  previewUnavailableReported = false;

  resetLightboxZoom();
  captionEl.textContent = "";
  image.removeAttribute("src");
  delete image.dataset.imageId;
  image.alt = "";
  image.hidden = false;
  errorEl.textContent = "图片无法加载";
  errorEl.hidden = true;
  loadingEl.hidden = true;
  originalButton.hidden = true;
  originalButton.disabled = false;
  originalButton.textContent = "原图";
  artworksEl.replaceChildren();
  tagsEl.replaceChildren();
  metaEl.replaceChildren();
}

function lightboxViewportSize() {
  const style = window.getComputedStyle(stage);
  const paddingX =
    (parseFloat(style.paddingLeft) || 0) +
    (parseFloat(style.paddingRight) || 0);
  const paddingY =
    (parseFloat(style.paddingTop) || 0) +
    (parseFloat(style.paddingBottom) || 0);
  return {
    width: Math.max(0, stage.clientWidth - paddingX),
    height: Math.max(0, stage.clientHeight - paddingY),
  };
}

function clampLightboxZoom() {
  const viewport = lightboxViewportSize();
  const imageWidth = image.offsetWidth * zoom.scale;
  const imageHeight = image.offsetHeight * zoom.scale;
  const maxX = Math.max(0, (imageWidth - viewport.width) / 2);
  const maxY = Math.max(0, (imageHeight - viewport.height) / 2);
  zoom.x = Math.min(maxX, Math.max(-maxX, zoom.x));
  zoom.y = Math.min(maxY, Math.max(-maxY, zoom.y));
}

function applyLightboxZoom() {
  image.style.transform = `translate(${zoom.x}px, ${zoom.y}px) scale(${zoom.scale})`;
  stage.classList.toggle(
    "is-zoomed",
    zoom.scale > ZOOM_MIN + 0.001,
  );
}

function resetLightboxZoom() {
  zoom.scale = ZOOM_MIN;
  zoom.x = 0;
  zoom.y = 0;
  zoom.dragging = false;
  zoom.pointerId = null;
  if (image) {
    image.style.transform = "";
  }
  if (stage) {
    stage.classList.remove("is-zoomed", "is-dragging");
  }
}

function zoomLightboxAt(clientX, clientY, factor) {
  if (image.hidden || image.offsetWidth === 0) {
    return;
  }
  const nextScale = Math.min(
    ZOOM_MAX,
    Math.max(ZOOM_MIN, zoom.scale * factor),
  );
  if (Math.abs(nextScale - zoom.scale) < 0.0001) {
    return;
  }
  const rect = stage.getBoundingClientRect();
  const offsetX = clientX - (rect.left + rect.width / 2);
  const offsetY = clientY - (rect.top + rect.height / 2);
  const ratio = nextScale / zoom.scale;
  zoom.x = offsetX - (offsetX - zoom.x) * ratio;
  zoom.y = offsetY - (offsetY - zoom.y) * ratio;
  zoom.scale = nextScale;
  if (nextScale <= ZOOM_MIN + 0.001) {
    zoom.x = 0;
    zoom.y = 0;
  }
  clampLightboxZoom();
  applyLightboxZoom();
}

function beginLightboxDrag(event) {
  if (!dialog.open || zoom.scale <= ZOOM_MIN + 0.001 || event.button !== 0) {
    return;
  }
  event.preventDefault();
  zoom.dragging = true;
  zoom.pointerId = event.pointerId;
  zoom.startX = event.clientX;
  zoom.startY = event.clientY;
  zoom.originX = zoom.x;
  zoom.originY = zoom.y;
  stage.classList.add("is-dragging");
  try {
    image.setPointerCapture(event.pointerId);
  } catch {
    // 某些浏览器在 pointerdown 里不能立即捕获，忽略即可。
  }
}

function moveLightboxDrag(event) {
  if (!zoom.dragging || event.pointerId !== zoom.pointerId) {
    return;
  }
  event.preventDefault();
  zoom.x = zoom.originX + (event.clientX - zoom.startX);
  zoom.y = zoom.originY + (event.clientY - zoom.startY);
  clampLightboxZoom();
  applyLightboxZoom();
}

function endLightboxDrag(event) {
  if (!zoom.dragging) {
    return;
  }
  if (event && event.pointerId !== zoom.pointerId) {
    return;
  }
  zoom.dragging = false;
  zoom.pointerId = null;
  stage.classList.remove("is-dragging");
}

function init(userOptions) {
  options = {
    renderSidebar:
      userOptions && typeof userOptions.renderSidebar === "function"
        ? userOptions.renderSidebar
        : null,
    onImageUnavailable:
      userOptions && typeof userOptions.onImageUnavailable === "function"
        ? userOptions.onImageUnavailable
        : null,
  };
  ensureDom();
}

function open(nextEntries, startIndex) {
  ensureDom();
  if (!Array.isArray(nextEntries) || nextEntries.length === 0) {
    return;
  }
  entries = nextEntries.slice();
  const requested = Number.isInteger(startIndex) ? startIndex : 0;
  index = Math.min(entries.length - 1, Math.max(0, requested));

  renderLightbox();

  if (!dialog.open) {
    dialog.showModal();
    document.body.classList.add("lightbox-open");
  }
  closeButton.focus({ preventScroll: true });
}

function close() {
  if (dialog && dialog.open) {
    dialog.close();
  }
}

function isOpen() {
  return Boolean(dialog && dialog.open);
}

function refreshSidebar() {
  const entry = currentEntry();
  if (entry) {
    renderSidebar(entry);
  }
}

export const ImageLightbox = {
  init,
  open,
  close,
  isOpen,
  refreshSidebar,
};
