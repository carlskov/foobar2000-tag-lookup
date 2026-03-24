#include "tag_lookup_service.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace taglookup {
namespace {

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
  const size_t total_size = size * nmemb;
  auto* out = static_cast<std::string*>(userp);
  out->append(static_cast<char*>(contents), total_size);
  return total_size;
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

std::string BuildMusicBrainzUrl(CURL* curl, const LookupQuery& query, size_t limit, size_t offset,
                                bool strict) {
  std::ostringstream q;
  if (strict) {
    q << "recording:\"" << query.title << "\" AND artist:\"" << query.artist << "\"";
  } else {
    // Broad mode intentionally avoids quoting to match tokenized titles.
    q << "recording:" << query.title;
  }

  std::ostringstream url;
  url << "https://musicbrainz.org/ws/2/recording/?query="
      << UrlEncode(curl, q.str())
      << "&fmt=json&inc=artist-credits+releases"
      << "&limit=" << limit
      << "&offset=" << offset;
  return url.str();
}

std::string ExtractPrimaryArtist(const nlohmann::json& recording) {
  if (recording.contains("artist-credit") && recording["artist-credit"].is_array() &&
      !recording["artist-credit"].empty()) {
    return recording["artist-credit"][0].value("name", "");
  }
  return "";
}

int ReadScore(const nlohmann::json& recording) {
  if (!recording.contains("score")) {
    return 0;
  }
  if (recording["score"].is_number_integer()) {
    return recording["score"].get<int>();
  }
  if (recording["score"].is_string()) {
    try {
      return std::stoi(recording["score"].get<std::string>());
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

}  // namespace

std::vector<TagResult> TagLookupService::LookupAll(const LookupQuery& query, size_t limit) const {
  std::vector<TagResult> out;

  if (query.artist.empty() || query.title.empty()) {
    return out;
  }

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    return out;
  }

  size_t safe_limit = limit;
  if (safe_limit < 1) {
    safe_limit = 1;
  }
  if (safe_limit > 200) {
    safe_limit = 200;
  }

  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "foo_taglookup/0.1 (+https://github.com/yourname/foo_taglookup)");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

  std::unordered_set<std::string> seen;

  auto collect = [&](bool strict_query) {
    const size_t page_size = 100;
    size_t offset = 0;
    size_t total_available = static_cast<size_t>(-1);
    size_t page_guard = 0;

    while ((offset < total_available || total_available == static_cast<size_t>(-1)) &&
           out.size() < safe_limit) {
      std::string response;
      const std::string url = BuildMusicBrainzUrl(curl, query, page_size, offset, strict_query);

      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

      const CURLcode res = curl_easy_perform(curl);
      if (res != CURLE_OK || response.empty()) {
        break;
      }

      nlohmann::json json;
      try {
        json = nlohmann::json::parse(response);
      } catch (...) {
        break;
      }

      if (json.contains("recording-count")) {
        if (json["recording-count"].is_number_unsigned()) {
          total_available = json["recording-count"].get<size_t>();
        } else if (json["recording-count"].is_number_integer()) {
          const auto count = json["recording-count"].get<long long>();
          if (count >= 0) {
            total_available = static_cast<size_t>(count);
          }
        } else if (json["recording-count"].is_string()) {
          try {
            total_available = static_cast<size_t>(std::stoull(json["recording-count"].get<std::string>()));
          } catch (...) {
            // Keep unknown total and rely on page size termination.
          }
        }
      }

      if (!json.contains("recordings") || !json["recordings"].is_array() || json["recordings"].empty()) {
        break;
      }

      const auto& recordings = json["recordings"];
      const size_t fetched_count = recordings.size();

      for (const auto& recording : recordings) {
        TagResult base;
        base.title = recording.value("title", "");
        base.artist = ExtractPrimaryArtist(recording);
        base.recording_id = recording.value("id", "");
        base.score = ReadScore(recording);

        if (base.artist.empty() || base.title.empty()) {
          continue;
        }

        if (recording.contains("releases") && recording["releases"].is_array() &&
            !recording["releases"].empty()) {
          for (const auto& release : recording["releases"]) {
            TagResult item = base;
            item.album = release.value("title", "");
            item.date = release.value("date", "");
            item.release_id = release.value("id", "");

            const std::string key = item.recording_id + "|" + item.release_id;
            if (!key.empty() && seen.insert(key).second) {
              out.push_back(std::move(item));
            }

            if (out.size() >= safe_limit) {
              break;
            }
          }
        } else {
          const std::string key = base.recording_id + "|";
          if (!base.recording_id.empty() && seen.insert(key).second) {
            out.push_back(std::move(base));
          }
        }

        if (out.size() >= safe_limit) {
          break;
        }
      }

      offset += page_size;
      if (fetched_count < page_size) {
        break;
      }

      ++page_guard;
      if (page_guard >= 20) {
        break;
      }
    }
  };

  collect(true);
  if (out.size() < 5) {
    collect(false);
  }

  curl_easy_cleanup(curl);

  std::sort(out.begin(), out.end(), [](const TagResult& a, const TagResult& b) {
    if (a.score != b.score) {
      return a.score > b.score;
    }
    if (a.artist != b.artist) {
      return a.artist < b.artist;
    }
    if (a.title != b.title) {
      return a.title < b.title;
    }
    return a.album < b.album;
  });

  return out;
}

}  // namespace taglookup
