#include "lobkit/md/status.hpp"

namespace lobkit::md {

const char* to_string(ParseStatus s) noexcept {
  switch (s) {
    case ParseStatus::Ok:
      return "Ok";
    case ParseStatus::End:
      return "End";
    case ParseStatus::Truncated:
      return "Truncated";
    case ParseStatus::ZeroLength:
      return "ZeroLength";
    case ParseStatus::BadLength:
      return "BadLength";
    case ParseStatus::BadField:
      return "BadField";
    case ParseStatus::BadType:
      return "BadType";
    case ParseStatus::BadTimestamp:
      return "BadTimestamp";
    case ParseStatus::Overflow:
      return "Overflow";
    case ParseStatus::BadLevelCount:
      return "BadLevelCount";
  }
  return "?";
}

}  // namespace lobkit::md

#include "lobkit/md/replay.hpp"

namespace lobkit::md {

const char* to_string(DivergenceReason r) noexcept {
  switch (r) {
    case DivergenceReason::None:
      return "None";
    case DivergenceReason::RowMismatch:
      return "RowMismatch";
    case DivergenceReason::FeedProblem:
      return "FeedProblem";
    case DivergenceReason::FieldMismatch:
      return "FieldMismatch";
    case DivergenceReason::RowsExhausted:
      return "RowsExhausted";
    case DivergenceReason::RowParseError:
      return "RowParseError";
    case DivergenceReason::MessageParseError:
      return "MessageParseError";
  }
  return "?";
}

}  // namespace lobkit::md
