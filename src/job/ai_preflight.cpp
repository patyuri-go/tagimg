#include "job/ai_preflight.hpp"

#include "job/job.hpp"

namespace tagimg {

bool AiPreflight::ensure(JobContext &context) {
  if (state_ == State::Unknown) {
    const auto health = context.ai_health.get().check();
    state_ = health && health->ready() ? State::Ready : State::Unavailable;
  }
  return state_ == State::Ready;
}

bool AiPreflight::checked() const noexcept {
  return state_ != State::Unknown;
}

bool AiPreflight::available() const noexcept {
  return state_ == State::Ready;
}

} // namespace tagimg
