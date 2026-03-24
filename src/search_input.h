#pragma once

#include "tag_lookup_service.h"

#include <optional>
#include <string>

namespace taglookup {

std::optional<LookupQuery> PromptForLookupQuery(const LookupQuery& seed,
												const std::string& statusMessage = "");

}  // namespace taglookup
