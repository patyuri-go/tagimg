// 可复用的确认弹窗。无构建步骤，直接暴露 window.ConfirmDialog。
//
//   const ok = await ConfirmDialog.open({
//     title: "删除作品",
//     message: "确定删除吗？",
//     confirmText: "删除",
//     cancelText: "取消",
//     danger: true,
//   });
//
// Esc、点击遮罩和取消按钮都返回 false；关闭后焦点回到触发元素。
(function () {
  "use strict";

  let dialog = null;
  let titleEl = null;
  let messageEl = null;
  let cancelButton = null;
  let confirmButton = null;
  let pending = null;
  let lastFocused = null;

  function ensureDialog() {
    if (dialog) {
      return;
    }

    dialog = document.createElement("dialog");
    dialog.className = "dialog confirm-dialog";

    const form = document.createElement("div");
    form.className = "dialog-form";

    const head = document.createElement("header");
    head.className = "dialog-head";
    titleEl = document.createElement("h2");
    titleEl.id = "confirm-dialog-title";
    head.appendChild(titleEl);

    const body = document.createElement("div");
    body.className = "dialog-body";
    messageEl = document.createElement("p");
    messageEl.className = "confirm-dialog-message";
    messageEl.id = "confirm-dialog-message";
    body.appendChild(messageEl);

    const foot = document.createElement("footer");
    foot.className = "dialog-foot";
    cancelButton = document.createElement("button");
    cancelButton.type = "button";
    cancelButton.className = "button ghost";
    confirmButton = document.createElement("button");
    confirmButton.type = "button";
    foot.append(cancelButton, confirmButton);

    form.append(head, body, foot);
    dialog.appendChild(form);
    dialog.setAttribute("aria-labelledby", titleEl.id);
    dialog.setAttribute("aria-describedby", messageEl.id);
    document.body.appendChild(dialog);

    cancelButton.addEventListener("click", () => finish(false));
    confirmButton.addEventListener("click", () => finish(true));

    // Esc 默认会直接关闭 dialog，这里改成走统一的 Promise 收尾。
    dialog.addEventListener("cancel", (event) => {
      event.preventDefault();
      finish(false);
    });

    // 点击遮罩（dialog 自身而不是内容）视为取消。
    dialog.addEventListener("click", (event) => {
      if (event.target === dialog) {
        finish(false);
      }
    });
  }

  function finish(value) {
    if (!pending) {
      return;
    }
    const resolve = pending;
    pending = null;
    if (dialog && dialog.open) {
      dialog.close();
    }
    if (lastFocused && lastFocused.isConnected) {
      lastFocused.focus();
    }
    lastFocused = null;
    resolve(value);
  }

  function open(options) {
    ensureDialog();
    if (pending) {
      // 同一时间只允许一个确认弹窗；重复打开视为取消上一次。
      finish(false);
    }

    const settings = options || {};
    titleEl.textContent = settings.title || "确认操作";
    messageEl.textContent = settings.message || "";
    cancelButton.textContent = settings.cancelText || "取消";
    confirmButton.textContent = settings.confirmText || "确定";

    const danger = Boolean(settings.danger);
    confirmButton.className = danger ? "button danger" : "button primary";

    lastFocused = document.activeElement;
    const promise = new Promise((resolve) => {
      pending = resolve;
    });

    dialog.showModal();
    // 默认焦点放在取消上，避免回车误触危险操作。
    cancelButton.focus();
    return promise;
  }

  window.ConfirmDialog = { open };
})();
