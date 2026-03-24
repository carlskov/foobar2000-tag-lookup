#pragma once

#include "tag_lookup_service.h"

#include <optional>
#include <vector>

namespace taglookup {

std::optional<size_t> SelectTagResultIndex(const LookupQuery& query,
                                           const std::vector<TagResult>& matches);

}  // namespace taglookup
