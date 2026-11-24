// 标签输入联想：按路径逐段解析，返回 tagId、name 和完整路径。
const TAG_SEARCH_QUERY = `
  query SearchTags($query: String!, $scope: TagSearchScope, $limit: Int!) {
    tags(query: $query, scope: $scope, limit: $limit) {
      tagId
      name
      tagPath
    }
  }
`;

// 空输入或停在某个父路径时，用 tagChildren 列出下一层标签。
// tags 查询要求关键词非空，不能用来做“点击就展开”。
const TAG_CHILDREN_QUERY = `
  query TagChildren($parentId: ID) {
    tagChildren(parentId: $parentId) {
      id
      name
      path
    }
  }
`;

// 路径解析缓存：完整路径 -> 标签，父路径 + 段名 -> 标签。
// 多个输入框共享缓存，同一个路径只解析一次。
const pathCache = new Map();
const childCache = new Map();

async function postGraphQL(query, variables, signal) {
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
    throw new Error(payload.errors[0].message || "标签查询失败");
  }
  return payload.data || {};
}

export async function fetchTags(text, scope, limit, signal) {
  const data = await postGraphQL(
    TAG_SEARCH_QUERY,
    { query: text, scope, limit },
    signal,
  );
  return data.tags || [];
}

async function fetchTagChildren(parentId, signal) {
  const data = await postGraphQL(TAG_CHILDREN_QUERY, { parentId }, signal);
  return (data.tagChildren || []).map((item) => ({
    tagId: item.id,
    name: item.name,
    tagPath: item.path,
  }));
}

// 去掉末尾连续的 ::，得到真正要解析或暂存的路径。
export function trimPathSuffix(text) {
  return text.trim().replace(/(?:::\s*)+$/, "");
}

// 把 1::2::3 从根开始逐段解析成标签；路径不存在时返回 null。
export async function resolveTagPath(text, signal) {
  const normalized = trimPathSuffix(text);
  if (normalized === "") {
    return { id: null, name: "", path: "" };
  }
  if (pathCache.has(normalized)) {
    return pathCache.get(normalized);
  }

  const names = normalized.split("::");
  if (names.some((name) => name.trim() === "")) {
    pathCache.set(normalized, null);
    return null;
  }

  let parent = { id: null, name: "", path: "" };
  for (const rawName of names) {
    const name = rawName.trim();
    const key = `${parent.path}\u0000${name}`;
    let item = null;
    if (childCache.has(key)) {
      item = childCache.get(key);
    } else {
      // 只查直接子标签，保证补全出来的是路径的下一段。
      const scope = parent.id
        ? { parentId: parent.id, directOnly: true }
        : {};
      const items = await fetchTags(name, scope, 10, signal);
      item =
        items.find(
          (entry) => entry.name.toLowerCase() === name.toLowerCase(),
        ) || null;
      childCache.set(key, item);
    }
    if (!item) {
      pathCache.set(normalized, null);
      return null;
    }
    parent = { id: item.tagId, name: item.name, path: item.tagPath };
  }

  pathCache.set(normalized, parent);
  return parent;
}

// 拆成「已确定父路径」和「正在输入的最后一段」。
export function splitTagInput(text) {
  const value = text.trimEnd();
  if (value.endsWith("::")) {
    return { parentText: value.slice(0, -2).trimEnd(), partial: "" };
  }
  const index = value.lastIndexOf("::");
  if (index < 0) {
    return { parentText: "", partial: value };
  }
  return {
    parentText: value.slice(0, index).trimEnd(),
    partial: value.slice(index + 2),
  };
}

// 给一个输入框绑定路径补全、键盘选择和下拉渲染。
// 回车调用 onAdd，Shift+回车调用 onRemove；两者由使用方决定业务语义。
// filterItem 可选，返回 false 的候选不会显示，也不会被键盘选中。
export function createTagSearch(options) {
  const { input, list } = options;
  const idPrefix = options.idPrefix || "tag";
  const emptyText = options.emptyText || "没有匹配的标签";
  const filterItem = options.filterItem || (() => true);
  let timer = 0;
  let controller = null;
  let composing = false;
  let suggestions = [];
  let active = -1;
  // 最近一次点击候选选中的标签，Tab/点击补全时复用。
  let chosen = null;
  // 提交令牌：切换图片、关闭输入框或开始新提交时，旧解析结果必须失效。
  let submitToken = 0;
  let submitController = null;
  let submitting = false;

  function closeList() {
    clearTimeout(timer);
    timer = 0;
    if (controller) {
      controller.abort();
      controller = null;
    }
    list.hidden = true;
    list.replaceChildren();
    suggestions = [];
    active = -1;
    input.setAttribute("aria-expanded", "false");
    input.removeAttribute("aria-activedescendant");
  }

  function cancelSubmit() {
    submitToken += 1;
    submitting = false;
    if (submitController) {
      submitController.abort();
      submitController = null;
    }
  }

  // 对外 close 同时取消正在解析的提交；内部只关下拉。
  function close() {
    cancelSubmit();
    closeList();
  }

  function setActive(index) {
    const items = list.querySelectorAll('[role="option"]');
    if (index < 0 || index >= items.length) {
      return;
    }
    active = index;
    items.forEach((item, position) => {
      const selected = position === index;
      item.classList.toggle("active", selected);
      item.setAttribute("aria-selected", selected ? "true" : "false");
      if (selected) {
        item.scrollIntoView({ block: "nearest" });
      }
    });
    input.setAttribute("aria-activedescendant", items[index].id);
  }

  // 补全候选：完整路径写回输入框，并补 :: 方便继续向下补全。
  function choose(index) {
    const item = suggestions[index];
    if (!item) {
      return;
    }
    input.value = `${item.tagPath}::`;
    chosen = { id: item.tagId, name: item.name, path: item.tagPath };
    closeList();
    input.focus();
    input.setSelectionRange(input.value.length, input.value.length);
    if (options.onChoose) {
      options.onChoose(chosen);
    }
  }

  function render(items, text) {
    suggestions = items;
    active = -1;

    const fragment = document.createDocumentFragment();
    items.forEach((item, index) => {
      const option = document.createElement("li");
      option.id = `${idPrefix}-option-${index}`;
      option.className = "tag-option";
      option.setAttribute("role", "option");
      option.setAttribute("aria-selected", "false");

      const name = document.createElement("span");
      name.className = "tag-option-name";
      name.textContent = item.name || "（无名称）";

      const path = document.createElement("span");
      path.className = "tag-option-path";
      path.textContent = item.tagPath || "";

      option.append(name, path);
      // pointerdown 先于 blur，阻止默认行为可以保住输入框焦点。
      option.addEventListener("pointerdown", (event) => {
        event.preventDefault();
        choose(index);
      });
      fragment.appendChild(option);
    });

    if (items.length === 0) {
      const empty = document.createElement("li");
      empty.className = "tag-empty";
      empty.textContent = text || emptyText;
      fragment.appendChild(empty);
    }

    list.replaceChildren(fragment);
    list.hidden = false;
    input.setAttribute("aria-expanded", "true");
  }

  async function refresh(text, signal) {
    const { parentText, partial } = splitTagInput(text);

    // 没有父路径时全库搜索；有父路径时在该标签的整棵子树里搜索，
    // 这样即使跳过中间层，也能补全出完整路径。
    let parent = { id: null, path: "" };
    if (parentText) {
      parent = await resolveTagPath(parentText, signal);
      if (signal.aborted) {
        return;
      }
      if (!parent) {
        render([], "路径不存在");
        return;
      }
    }

    // 空输入列出顶层标签；停在 parent:: 时列出下一层。
    // 这样点击输入框也能立即看到候选，而不是必须先改一次输入。
    if (partial === "") {
      const items = await fetchTagChildren(parent.id, signal);
      if (signal.aborted) {
        return;
      }
      render(items.filter((item) => filterItem(item)).slice(0, 10));
      return;
    }

    const scope = parent.id ? { parentId: parent.id } : undefined;
    const items = await fetchTags(partial, scope, 10, signal);
    if (signal.aborted) {
      return;
    }
    render(items.filter((item) => filterItem(item)));
  }

  function handleInput() {
    if (composing || submitting) {
      return;
    }
    // 手动改过输入框后，之前点击选中的标签不再可信。
    chosen = null;
    clearTimeout(timer);
    timer = 0;
    if (controller) {
      controller.abort();
      controller = null;
    }

    const text = input.value.trim();
    if (!text) {
      closeList();
      return;
    }

    timer = setTimeout(() => {
      timer = 0;
      const next = new AbortController();
      controller = next;
      refresh(text, next.signal).catch((error) => {
        if (error && error.name === "AbortError") {
          return;
        }
        if (controller !== next || next.signal.aborted) {
          return;
        }
        render([], "标签查询失败");
      });
    }, 200);
  }

  // 点击输入框立即刷新，不受 200ms 输入防抖限制。
  // 空输入时 refresh 会列出顶层标签。
  function openSuggestions() {
    if (composing || submitting) {
      return;
    }
    clearTimeout(timer);
    timer = 0;
    if (controller) {
      controller.abort();
      controller = null;
    }
    const text = input.value.trim();
    const next = new AbortController();
    controller = next;
    refresh(text, next.signal).catch((error) => {
      if (error && error.name === "AbortError") {
        return;
      }
      if (controller !== next || next.signal.aborted) {
        return;
      }
      render([], "标签查询失败");
    });
  }

  function activeTag() {
    if (list.hidden || active < 0) {
      return null;
    }
    const item = suggestions[active];
    if (!item) {
      return null;
    }
    return { id: item.tagId, name: item.name, path: item.tagPath };
  }

  function reportError(error) {
    if (typeof options.onError !== "function") {
      return;
    }
    const message =
      typeof error === "string"
        ? error
        : error && error.message
          ? error.message
          : "标签操作失败";
    options.onError(message);
  }

  function finishSubmit(token, success) {
    if (token !== submitToken) {
      return;
    }
    if (!success) {
      closeList();
      return;
    }
    input.value = "";
    chosen = null;
    closeList();
    input.focus();
    input.setSelectionRange(0, 0);
  }

  async function submitTag(action, tag) {
    const handler = action === "add" ? options.onAdd : options.onRemove;
    if (!handler || submitting || !tag || !tag.id) {
      return;
    }
    closeList();
    const token = ++submitToken;
    submitting = true;
    submitController = null;
    try {
      const result = await handler(tag);
      finishSubmit(token, result === true);
    } catch (error) {
      if (token !== submitToken) {
        return;
      }
      closeList();
      reportError(error && error.message ? error.message : "标签操作失败");
    } finally {
      if (token === submitToken) {
        submitting = false;
      }
    }
  }

  async function submitInput(action, text) {
    const handler = action === "add" ? options.onAdd : options.onRemove;
    if (!handler || submitting) {
      return;
    }
    const normalized = trimPathSuffix(text);
    if (normalized === "") {
      return;
    }
    closeList();
    const token = ++submitToken;
    submitting = true;
    const next = new AbortController();
    submitController = next;
    try {
      const tag = await resolveTagPath(normalized, next.signal);
      if (token !== submitToken) {
        return;
      }
      if (!tag || !tag.id) {
        reportError(`找不到标签路径：${normalized}`);
        return;
      }
      const result = await handler(tag);
      finishSubmit(token, result === true);
    } catch (error) {
      if (token !== submitToken) {
        return;
      }
      if (error && error.name === "AbortError") {
        return;
      }
      closeList();
      reportError(error && error.message ? error.message : "标签解析失败");
    } finally {
      if (token === submitToken) {
        submitting = false;
        submitController = null;
      }
    }
  }

  // 中文输入法组合期间不触发查询或回车。
  input.addEventListener("compositionstart", () => {
    composing = true;
  });

  input.addEventListener("compositionend", () => {
    composing = false;
  });

  input.addEventListener("input", handleInput);
  input.addEventListener("click", openSuggestions);

  input.addEventListener("keydown", (event) => {
    if (event.isComposing || composing) {
      return;
    }

    if (event.key === "Enter") {
      event.preventDefault();
      if (submitting) {
        return;
      }
      if (event.shiftKey) {
        if (!options.onRemove) {
          return;
        }
        const tag = activeTag();
        if (tag) {
          submitTag("remove", tag);
        } else {
          submitInput("remove", input.value);
        }
        return;
      }
      if (options.onAdd) {
        const tag = activeTag();
        if (tag) {
          submitTag("add", tag);
        } else {
          submitInput("add", input.value);
        }
        return;
      }
      // 没有添加回调时，回车只承担补全职责。
      if (!list.hidden && active >= 0) {
        choose(active);
      }
      return;
    }

    if (event.key === "Tab") {
      if (!list.hidden && active >= 0) {
        event.preventDefault();
        choose(active);
      }
      return;
    }

    if (list.hidden) {
      return;
    }
    const count = suggestions.length;
    if (event.key === "ArrowDown") {
      event.preventDefault();
      if (count > 0) {
        setActive((active + 1) % count);
      }
    } else if (event.key === "ArrowUp") {
      event.preventDefault();
      if (count > 0) {
        setActive(active <= 0 ? count - 1 : active - 1);
      }
    } else if (event.key === "Escape") {
      event.preventDefault();
      closeList();
    }
  });

  input.addEventListener("blur", closeList);

  return {
    close,
    choose,
    getChosen: () => chosen,
    clearChosen: () => {
      chosen = null;
    },
    focus: () => input.focus(),
  };
}
