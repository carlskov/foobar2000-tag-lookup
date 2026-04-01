#pragma once

#include "tag_lookup_service.h"

#include <string>
#include <vector>

namespace taglookup {

enum class AlbumArtProvider {
  MusicBrainz,
  Discogs,
  AlbumArtExchange,
};

struct AlbumArtQuery {
  std::string artist;
  std::string album;
  std::string label;
  std::string title;
  std::string year;
  SearchMode search_mode = SearchMode::ExactPhrase;
  AlbumArtProvider provider = AlbumArtProvider::MusicBrainz;
};

struct AlbumArtCandidate {
  std::string url;
  std::string artist;
  std::string album;
  std::string label;
  std::string date;
  std::string provider_hint;
  std::string extension_hint;
};

class AlbumArtService {
 public:
  std::vector<AlbumArtCandidate> FindAlbumArt(const AlbumArtQuery& query,
                                              size_t limit = 10) const;

  bool DownloadBytes(const std::string& url, std::string& outBytes,
                     std::string& outContentType) const;

  static std::string GuessExtension(const std::string& url,
                                    const std::string& contentType,
                                    const std::string& extensionHint);
};

}  // namespace taglookup
