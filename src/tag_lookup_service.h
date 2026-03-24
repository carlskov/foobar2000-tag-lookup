#pragma once

#include <string>
#include <vector>

namespace taglookup {

enum class SearchMode {
  ExactPhrase,
  Tokenized,
};

enum class SearchProvider {
  MusicBrainz,
  Discogs,
};

struct LookupQuery {
  std::string artist;
  std::string album;
  std::string label;
  std::string title;
  std::string year;
  SearchMode search_mode = SearchMode::Tokenized;
  SearchProvider provider = SearchProvider::MusicBrainz;
};

struct TagResult {
  std::string artist;
  std::string title;
  std::string album;
  std::string label;
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
