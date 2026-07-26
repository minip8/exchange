#include "net/core/RejectCode.hpp"

namespace Exchange::Net {
std::string_view toString(RejectCode code) noexcept {
  switch (code) {
    case RejectCode::None:
      return "None";
    case RejectCode::NotLoggedOn:
      return "NotLoggedOn";
    case RejectCode::AlreadyLoggedOn:
      return "AlreadyLoggedOn";
    case RejectCode::AuthFailed:
      return "AuthFailed";
    case RejectCode::Throttled:
      return "Throttled";
    case RejectCode::MalformedMessage:
      return "MalformedMessage";
    case RejectCode::UnsupportedVersion:
      return "UnsupportedVersion";
    case RejectCode::UnknownBook:
      return "UnknownBook";
    case RejectCode::UnknownSymbol:
      return "UnknownSymbol";
    case RejectCode::DuplicateSymbol:
      return "DuplicateSymbol";
    case RejectCode::InvalidSymbol:
      return "InvalidSymbol";
    case RejectCode::InvalidQuantity:
      return "InvalidQuantity";
    case RejectCode::InvalidPrice:
      return "InvalidPrice";
    case RejectCode::DuplicateClientOrderId:
      return "DuplicateClientOrderId";
    case RejectCode::UnknownOrder:
      return "UnknownOrder";
    case RejectCode::NotYourOrder:
      return "NotYourOrder";
    case RejectCode::InternalError:
      return "InternalError";
  }
  return "?";
}
}  // namespace Exchange::Net
