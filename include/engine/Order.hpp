#pragma once

#include "types/OrderId.hpp"
namespace Engine {

class Order {
 public:
  Order() = delete;
  Order(const Order&) = delete;
  Order& operator=(const Order&) = delete;
  Order(Order&&) noexcept;
  Order& operator=(Order&&) noexcept;

  OrderId id() const { return m_id; };

 private:
  OrderId m_id;
};

}  // namespace Engine
