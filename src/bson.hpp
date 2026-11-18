#pragma once
// Internal: nevla::Value <-> BSON
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/types/bson_value/view.hpp>

#include "nevla/value.hpp"

namespace nevla::detail {
bsoncxx::document::value toBson(const Value &object);  // not an object -> empty document
Value fromBson(bsoncxx::document::view doc);
Value fromBson(const bsoncxx::types::bson_value::view &v);
}  // namespace nevla::detail
