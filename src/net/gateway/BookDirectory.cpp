#include "net/gateway/BookDirectory.hpp"

#include <cstdint>
#include <limits>

namespace Exchange::Net {
bool BookDirectory::add(const BookInfo& info) {
  if (info.id.value > std::numeric_limits<uint32_t>::max()) return false;
  // Ids are minted by the engine's counter and never reused, so this can only
  // fire on a caller bug — but registering twice would orphan the first entry.
  if (m_by_id.contains(info.id)) return false;
  const std::size_t index{m_books.size()};
  m_books.push_back(info);
  m_by_id.emplace(info.id, index);
  return true;
}

const BookInfo* BookDirectory::find(OrderBookId id) const {
  const auto it{m_by_id.find(id)};
  return it == m_by_id.end() ? nullptr : &m_books[it->second];
}
}  // namespace Exchange::Net
