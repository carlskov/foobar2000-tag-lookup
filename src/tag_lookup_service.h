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
  SearchMode search_mode = SearchMode::ExactPhrase;
  SearchProvider provider = SearchProvider::MusicBrainz;
  bool overwrite_title_on_propagation = false;
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

// Per-track data resolved from the release's full tracklist.
struct TrackInfo {
  std::string title;
  std::string artist;  // empty means same as album artist
  std::string trackNumber;
  std::string discNumber;
  std::string mediaType;
};

// Result of a full tracklist fetch.
struct TracklistResult {
  std::string albumArtist;        // release-level artist; empty if not resolved
  std::vector<TrackInfo> tracks;  // ordered by track position in the release
};

class TagLookupService {
 public:
  std::vector<TagResult> LookupAll(const LookupQuery& query, size_t limit = 25) const;

  // Fetches the full ordered tracklist for the release (title + per-track artist).
  // Also resolves the album-level artist. One HTTP call for Discogs, one for MusicBrainz.
  TracklistResult FetchTracklist(const LookupQuery& query, const TagResult& result) const;

  // Legacy single-value helpers kept for compatibility.
  std::string ResolvePropagationTitle(const LookupQuery& query, const TagResult& result) const;
  std::string ResolvePropagationArtist(const LookupQuery& query, const TagResult& result) const;
};

}  // namespace taglookup
