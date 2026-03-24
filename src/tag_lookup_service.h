#pragma once

#include <optional>
#include <string>

namespace taglookup {

struct LookupQuery {
  std::string artist;
  std::string title;
};

struct TagResult {
  std::string artist;
  std::string title;
  std::string album;
  std::string date;
};

class TagLookupService {
 public:
  std::optional<TagResult> Lookup(const LookupQuery& query) const;
};

}  // namespace taglookup
