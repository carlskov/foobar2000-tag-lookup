#pragma once

#include <string>
#include <vector>

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
  std::string recording_id;
  std::string release_id;
  int score = 0;
};

class TagLookupService {
 public:
  std::vector<TagResult> LookupAll(const LookupQuery& query, size_t limit = 25) const;
};

}  // namespace taglookup
