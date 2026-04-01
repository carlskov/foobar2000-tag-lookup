#include "album_art_service.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace taglookup {
namespace {

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
  const size_t totalSize = size * nmemb;
  auto* out = static_cast<std::string*>(userp);
  out->append(static_cast<char*>(contents), totalSize);
  return totalSize;
}

std::string UrlEncode(CURL* curl, const std::string& input) {
  char* encoded = curl_easy_escape(curl, input.c_str(), static_cast<int>(input.size()));
  if (encoded == nullptr) {
    throw std::runtime_error("curl_easy_escape failed");
  }
  std::string result(encoded);
  curl_free(encoded);
  return result;
}

bool PerformJsonRequest(CURL* curl, const std::string& url, nlohmann::json& outJson,
                        struct curl_slist* headers = nullptr) {
  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  const CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK || response.empty()) {
    return false;
  }

  try {
    outJson = nlohmann::json::parse(response);
    return true;
  } catch (...) {
    return false;
  }
}

std::string ToLower(std::string v) {
  std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return v;
}

bool LooksLikeImageBytes(const std::string& bytes) {
  if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
      static_cast<unsigned char>(bytes[1]) == 0xD8 &&
      static_cast<unsigned char>(bytes[2]) == 0xFF) {
    return true;
  }
  if (bytes.size() >= 8 &&
      static_cast<unsigned char>(bytes[0]) == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' &&
      bytes[3] == 'G' && static_cast<unsigned char>(bytes[4]) == 0x0D &&
      static_cast<unsigned char>(bytes[5]) == 0x0A &&
      static_cast<unsigned char>(bytes[6]) == 0x1A &&
      static_cast<unsigned char>(bytes[7]) == 0x0A) {
    return true;
  }
  if (bytes.size() >= 12 && bytes.compare(0, 4, "RIFF") == 0 &&
      bytes.compare(8, 4, "WEBP") == 0) {
    return true;
  }
  return false;
}

std::string BuildMusicBrainzUrl(CURL* curl, const AlbumArtQuery& query, size_t limit,
                                SearchMode mode) {
  std::ostringstream q;
  bool haveAny = false;
  auto addClause = [&](const std::string& clause) {
    if (clause.empty()) {
      return;
    }
    if (haveAny) {
      q << " AND ";
    }
    q << clause;
    haveAny = true;
  };

  auto encodeField = [&](const char* key, const std::string& value) {
    if (value.empty()) {
      return;
    }
    if (mode == SearchMode::ExactPhrase) {
      addClause(std::string(key) + ":\"" + value + "\"");
    } else {
      addClause(std::string(key) + ":" + value);
    }
  };

  encodeField("artist", query.artist);
  encodeField("release", query.album);
  encodeField("track", query.title);
  encodeField("label", query.label);
  if (!query.year.empty()) {
    addClause("date:" + query.year);
  }

  if (!haveAny) {
    q << "release:\"\"";
  }

  std::ostringstream url;
  url << "https://musicbrainz.org/ws/2/release/?query=" << UrlEncode(curl, q.str())
      << "&fmt=json&inc=cover-art-archive&limit=" << limit;
  return url.str();
}

std::string BuildDiscogsSearchUrl(CURL* curl, const AlbumArtQuery& query, size_t limit,
                                  SearchMode mode, bool includeTrack, bool useGeneralQuery,
                                  const char* discogsType = "master") {
  std::ostringstream url;
  url << "https://api.discogs.com/database/search?type=" << discogsType << "&per_page=" << limit << "&page=1";

  auto appendParam = [&](const char* key, const std::string& value, bool quoted) {
    if (value.empty()) {
      return;
    }
    (void)quoted;
    std::string v = value;
    url << "&" << key << "=" << UrlEncode(curl, v);
  };

  appendParam("artist", query.artist, true);
  appendParam("release_title", query.album, true);
  if (includeTrack) {
    appendParam("track", query.title, true);
  }
  appendParam("label", query.label, true);
  appendParam("year", query.year, false);

  if (useGeneralQuery) {
    std::string combined;
    auto append = [&](const std::string& value) {
      if (value.empty()) {
        return;
      }
      if (!combined.empty()) {
        combined += ' ';
      }
      combined += value;
    };
    append(query.artist);
    append(query.album);
    append(query.title);
    append(query.label);
    append(query.year);
    appendParam("q", combined, false);
  }

  return url.str();
}

std::string ExtensionFromUrl(const std::string& url) {
  const size_t q = url.find('?');
  const std::string path = (q == std::string::npos) ? url : url.substr(0, q);
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) {
    return "";
  }
  const std::string ext = ToLower(path.substr(dot));
  if (ext == ".jpg" || ext == ".jpeg") {
    return ".jpg";
  }
  if (ext == ".png" || ext == ".webp") {
    return ext;
  }
  return "";
}

std::string JsonToString(const nlohmann::json& value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_number_integer()) {
    return std::to_string(value.get<long long>());
  }
  if (value.is_number_unsigned()) {
    return std::to_string(value.get<unsigned long long>());
  }
  if (value.is_number_float()) {
    return std::to_string(value.get<double>());
  }
  return "";
}

std::string JsonFieldToString(const nlohmann::json& object, const char* key) {
  if (!object.contains(key)) {
    return "";
  }
  return JsonToString(object[key]);
}

std::string JoinJsonStringArray(const nlohmann::json& value) {
  if (!value.is_array()) {
    return "";
  }

  std::string out;
  for (const auto& item : value) {
    const std::string text = JsonToString(item);
    if (text.empty()) {
      continue;
    }
    if (!out.empty()) {
      out += "; ";
    }
    out += text;
  }
  return out;
}

std::string JoinArtistNames(const nlohmann::json& artists) {
  if (!artists.is_array()) {
    return "";
  }

  std::string joined;
  for (const auto& artist : artists) {
    const std::string name = JsonFieldToString(artist, "name");
    if (name.empty()) {
      continue;
    }
    if (!joined.empty()) {
      joined += ", ";
    }
    joined += name;
  }
  return joined;
}

std::string ExtractDiscogsMasterCoverUrl(const nlohmann::json& master) {
  if (!master.contains("images") || !master["images"].is_array() || master["images"].empty()) {
    return "";
  }

  const auto& first = master["images"][0];
  std::string image = JsonFieldToString(first, "uri");
  if (image.empty()) {
    image = JsonFieldToString(first, "uri150");
  }
  return image;
}

std::string BuildAlbumArtExchangeSearchUrl(CURL* curl, const AlbumArtQuery& query,
                                           size_t page) {
  std::string combined;
  auto append = [&](const std::string& value) {
    if (value.empty()) {
      return;
    }
    if (!combined.empty()) {
      combined += ' ';
    }
    combined += value;
  };
  append(query.artist);
  append(query.album);
  append(query.title);
  append(query.label);
  append(query.year);

  std::ostringstream url;
  url << "https://albumartexchange.com/covers?q="
      << UrlEncode(curl, combined) << "&fltr=ARTISTTITLE&page=" << page;
  return url.str();
}

std::string ExtractCoverIdFromHtml(const std::string& html, size_t& pos) {
  const std::string searchStart = "data-coverid=\"";
  pos = html.find(searchStart, pos);
  if (pos == std::string::npos) {
    return "";
  }
  pos += searchStart.size();
  size_t end = html.find('"', pos);
  if (end == std::string::npos) {
    return "";
  }
  return html.substr(pos, end - pos);
}

std::string ExtractFullImageUrlFromCoverPage(CURL* curl, const std::string& coverId,
                                              const std::string& slug) {
  std::string detailUrl = "https://albumartexchange.com/covers/" + coverId;
  if (!slug.empty()) {
    detailUrl += "-" + slug;
  }

  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, detailUrl.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  struct curl_slist* headers = curl_slist_append(nullptr, "User-Agent: Mozilla/5.0");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  const CURLcode res = curl_easy_perform(curl);
  curl_slist_free_all(headers);

  if (res != CURLE_OK || response.empty()) {
    return "";
  }

  const std::string imgSearch = "<img src=\"/coverart/gallery/";
  size_t imgPos = response.find(imgSearch);
  if (imgPos == std::string::npos) {
    return "";
  }
  imgPos += imgSearch.size();
  size_t imgEnd = response.find('"', imgPos);
  if (imgEnd == std::string::npos) {
    return "";
  }

  std::string path = response.substr(imgPos, imgEnd - imgPos);
  if (path.find("placeholder") != std::string::npos) {
    return "";
  }

  return "https://albumartexchange.com/coverart/gallery/" + path;
}

std::string ExtractSlugFromDetailLink(const std::string& link) {
  size_t slash = link.rfind('/');
  if (slash == std::string::npos) {
    return "";
  }
  std::string filename = link.substr(slash + 1);
  size_t dash = filename.find('-');
  if (dash == std::string::npos) {
    return "";
  }
  return filename.substr(dash + 1);
}

std::vector<AlbumArtCandidate> FindMatchingAlbumArtExchangeCovers(CURL* curl,
                                                                   const AlbumArtQuery& query,
                                                                   size_t limit) {
  std::vector<AlbumArtCandidate> out;
  std::string response;
  std::string searchUrl = BuildAlbumArtExchangeSearchUrl(curl, query, 1);

  curl_easy_setopt(curl, CURLOPT_URL, searchUrl.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  struct curl_slist* headers = curl_slist_append(nullptr, "User-Agent: Mozilla/5.0");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  const CURLcode res = curl_easy_perform(curl);
  curl_slist_free_all(headers);

  if (res != CURLE_OK || response.empty()) {
    return out;
  }

  size_t pos = 0;
  std::unordered_set<std::string> seenIds;
  while (out.size() < limit) {
    std::string coverId = ExtractCoverIdFromHtml(response, pos);
    if (coverId.empty()) {
      break;
    }
    if (!seenIds.insert(coverId).second) {
      continue;
    }

    std::string fullUrl = ExtractFullImageUrlFromCoverPage(curl, coverId, "");
    if (fullUrl.empty()) {
      continue;
    }

    AlbumArtCandidate c;
    c.url = fullUrl;
    c.artist = query.artist;
    c.album = query.album;
    c.label = query.label;
    c.date = query.year;
    c.provider_hint = "albumartexchange";
    c.extension_hint = ".jpg";
    out.push_back(std::move(c));
  }

  return out;
}

}  // namespace

std::vector<AlbumArtCandidate> AlbumArtService::FindAlbumArt(const AlbumArtQuery& query,
                                                             size_t limit) const {
  std::vector<AlbumArtCandidate> out;

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    return out;
  }

  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_USERAGENT,
                   "foo_taglookup_albumart/0.1 (+https://example.invalid/contact)");

  if (query.provider == AlbumArtProvider::MusicBrainz) {
    nlohmann::json payload;
    if (PerformJsonRequest(curl, BuildMusicBrainzUrl(curl, query, limit, query.search_mode),
                           payload) &&
        payload.contains("releases") && payload["releases"].is_array()) {
      for (const auto& rel : payload["releases"]) {
        const std::string releaseId = rel.value("id", "");
        if (releaseId.empty()) {
          continue;
        }
        bool hasFront = true;
        if (rel.contains("cover-art-archive") && rel["cover-art-archive"].is_object()) {
          const auto& caa = rel["cover-art-archive"];
          if (caa.contains("front") && caa["front"].is_boolean()) {
            hasFront = caa["front"].get<bool>();
          }
        }
        if (!hasFront) {
          continue;
        }
        AlbumArtCandidate c;
        c.url = "https://coverartarchive.org/release/" + releaseId + "/front";
        if (rel.contains("artist-credit") && rel["artist-credit"].is_array() &&
            !rel["artist-credit"].empty()) {
          c.artist = rel["artist-credit"][0].value("name", "");
        }
        c.album = rel.value("title", "");
        c.date = rel.value("date", "");
        if (rel.contains("label-info") && rel["label-info"].is_array() &&
            !rel["label-info"].empty() && rel["label-info"][0].contains("label") &&
            rel["label-info"][0]["label"].is_object()) {
          c.label = rel["label-info"][0]["label"].value("name", "");
        }
        c.provider_hint = "musicbrainz";
        c.extension_hint = ".jpg";
        out.push_back(std::move(c));
        if (out.size() >= limit) {
          break;
        }
      }
    }
  } else if (query.provider == AlbumArtProvider::Discogs) {
    const char* discogsToken = std::getenv("DISCOGS_TOKEN");
    struct curl_slist* headers = nullptr;
    if (discogsToken != nullptr && discogsToken[0] != '\0') {
      const std::string auth = std::string("Authorization: Discogs token=") + discogsToken;
      headers = curl_slist_append(headers, auth.c_str());
    }

    auto collectAttempt = [&](SearchMode attemptMode, bool includeTrack, bool useGeneralQuery,
                              const char* discogsType = "master") {
      nlohmann::json payload;
      if (!(PerformJsonRequest(
                curl,
                BuildDiscogsSearchUrl(curl, query, limit, attemptMode, includeTrack,
                                      useGeneralQuery, discogsType),
                payload, headers) &&
            payload.contains("results") && payload["results"].is_array())) {
        return;
      }

      for (const auto& rel : payload["results"]) {
        const std::string masterId = JsonFieldToString(rel, "id");
        if (masterId.empty()) {
          continue;
        }

        nlohmann::json masterJson;
        if (!PerformJsonRequest(curl, "https://api.discogs.com/masters/" + masterId, masterJson,
                                headers)) {
          continue;
        }

        std::string image = ExtractDiscogsMasterCoverUrl(masterJson);
        if (image.empty()) {
          continue;
        }
        AlbumArtCandidate c;
        c.url = image;
        const std::string combinedTitle = JsonFieldToString(rel, "title");
        const size_t sep = combinedTitle.find(" - ");
        if (sep != std::string::npos) {
          c.artist = combinedTitle.substr(0, sep);
          c.album = combinedTitle.substr(sep + 3);
        } else {
          c.artist = JoinArtistNames(masterJson.value("artists", nlohmann::json::array()));
          c.album = JsonFieldToString(masterJson, "title");
        }
        c.label = JoinJsonStringArray(rel.value("label", nlohmann::json::array()));
        c.date = JsonFieldToString(masterJson, "year");
        c.provider_hint = "discogs";
        c.extension_hint = ExtensionFromUrl(image);
        out.push_back(std::move(c));
        if (out.size() >= limit) {
          break;
        }
      }
    };

    auto tryDiscogsType = [&](const char* discogsType) {
      collectAttempt(query.search_mode, !query.album.empty() ? false : !query.title.empty(), false, discogsType);
      if (out.size() >= limit) return true;
      if (out.empty() && !query.title.empty()) {
        collectAttempt(query.search_mode, false, false, discogsType);
      }
      if (out.size() >= limit) return true;
      if (out.empty()) {
        collectAttempt(query.search_mode, false, true, discogsType);
      }
      if (out.size() >= limit) return true;
      if (query.search_mode == SearchMode::ExactPhrase) {
        collectAttempt(SearchMode::Tokenized, !query.album.empty() ? false : !query.title.empty(), false, discogsType);
        if (out.size() >= limit) return true;
        if (out.empty() && !query.title.empty()) {
          collectAttempt(SearchMode::Tokenized, false, false, discogsType);
        }
        if (out.size() >= limit) return true;
        if (out.empty()) {
          collectAttempt(SearchMode::Tokenized, false, true, discogsType);
        }
      }
      return out.size() >= limit;
    };

    if (!tryDiscogsType("master") && out.empty()) {
      tryDiscogsType("release");
    }

    if (headers != nullptr) {
      curl_slist_free_all(headers);
    }
  } else if (query.provider == AlbumArtProvider::AlbumArtExchange) {
    auto collectAaxAttempt = [&](const std::string& artist, const std::string& album,
                                 const std::string& title) {
      AlbumArtQuery attemptQuery = query;
      attemptQuery.artist = artist;
      attemptQuery.album = album;
      attemptQuery.title = title;
      auto results = FindMatchingAlbumArtExchangeCovers(curl, attemptQuery, limit);
      for (auto& r : results) {
        out.push_back(std::move(r));
      }
    };

    const bool haveAlbum = !query.album.empty();
    const bool haveTrack = !query.title.empty();

    if (haveAlbum) {
      collectAaxAttempt(query.artist, query.album, "");
    }
    if (out.empty() && haveTrack) {
      collectAaxAttempt(query.artist, "", query.title);
    }
    if (out.empty()) {
      collectAaxAttempt(query.artist, query.album, query.title);
    }
  }

  curl_easy_cleanup(curl);

  std::unordered_set<std::string> seen;
  std::vector<AlbumArtCandidate> deduped;
  deduped.reserve(out.size());
  for (auto& c : out) {
    if (c.url.empty()) {
      continue;
    }
    if (seen.insert(c.url).second) {
      deduped.push_back(std::move(c));
    }
  }

  return deduped;
}

bool AlbumArtService::DownloadBytes(const std::string& url, std::string& outBytes,
                                    std::string& outContentType) const {
  outBytes.clear();
  outContentType.clear();

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    return false;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outBytes);
  curl_easy_setopt(curl, CURLOPT_USERAGENT,
                   "foo_taglookup_albumart/0.1 (+https://example.invalid/contact)");

  const CURLcode res = curl_easy_perform(curl);
  long responseCode = 0;
  (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
  if (res != CURLE_OK || outBytes.empty() || responseCode < 200 || responseCode >= 300) {
    curl_easy_cleanup(curl);
    return false;
  }

  char* contentType = nullptr;
  if (curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &contentType) == CURLE_OK &&
      contentType != nullptr) {
    outContentType = contentType;
  }

  const std::string loweredType = ToLower(outContentType);
  if ((!loweredType.empty() && loweredType.find("image/") == std::string::npos) &&
      !LooksLikeImageBytes(outBytes)) {
    curl_easy_cleanup(curl);
    return false;
  }
  if (loweredType.empty() && !LooksLikeImageBytes(outBytes)) {
    curl_easy_cleanup(curl);
    return false;
  }

  curl_easy_cleanup(curl);
  return true;
}

std::string AlbumArtService::GuessExtension(const std::string& url,
                                            const std::string& contentType,
                                            const std::string& extensionHint) {
  if (!extensionHint.empty()) {
    return extensionHint;
  }

  const std::string loweredType = ToLower(contentType);
  if (loweredType.find("image/jpeg") != std::string::npos ||
      loweredType.find("image/jpg") != std::string::npos) {
    return ".jpg";
  }
  if (loweredType.find("image/png") != std::string::npos) {
    return ".png";
  }
  if (loweredType.find("image/webp") != std::string::npos) {
    return ".webp";
  }

  const std::string fromUrl = ExtensionFromUrl(url);
  if (!fromUrl.empty()) {
    return fromUrl;
  }
  return ".jpg";
}

}  // namespace taglookup
