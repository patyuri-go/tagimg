#pragma once

namespace tagimg {

struct JobContext;

// 每个 job 一份的 AI 预检状态。
//
// 第一次 ensure() 调用 /health，要求 tagger 与 CLIP 都就绪；结果缓存
// 在对象里，因此一次 add/rescan 中无论扫描多少张图片都只探测一次。
// 对象不能跨 job 复用，否则 AI 掉线后仍会沿用旧结果。
class AiPreflight {
public:
  // 返回 true 表示 AI 可用。探测失败会缓存为不可用。
  [[nodiscard]] bool ensure(JobContext &context);

  [[nodiscard]] bool checked() const noexcept;
  [[nodiscard]] bool available() const noexcept;

private:
  enum class State { Unknown, Ready, Unavailable };

  State state_ = State::Unknown;
};

} // namespace tagimg
