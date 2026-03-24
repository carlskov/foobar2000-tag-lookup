#include "tag_lookup_service.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace taglookup {
namespace {

bool FuzzyContainsNormalized(const std::string& haystack, const std::string& needle);

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
                                SearchMode mode) {
  std::ostringstream q;
  if (mode == SearchMode::ExactPhrase) {
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

    if (!query.title.empty()) {
      addClause("track:\"" + query.title + "\"");
    }
    if (!query.artist.empty()) {
      addClause("artist:\"" + query.artist + "\"");
    }
    if (!query.album.empty()) {
      addClause("release:\"" + query.album + "\"");
    }
    if (!query.label.empty()) {
      addClause("label:\"" + query.label + "\"");
    }
    if (!query.year.empty()) {
      addClause("date:" + query.year);
    }

    if (!haveAny) {
      q << "recording:\"\"";
    }
  } else {
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

    if (!query.title.empty()) {
      addClause("track:" + query.title);
    }
    if (!query.artist.empty()) {
      addClause("artist:" + query.artist);
    }
    if (!query.album.empty()) {
      addClause("release:" + query.album);
    }
    if (!query.label.empty()) {
      addClause("label:" + query.label);
    }
    if (!query.year.empty()) {
      addClause("date:" + query.year);
    }

    if (!haveAny) {
      q << query.title;
    }
  }

  std::ostringstream url;
  url << "https://musicbrainz.org/ws/2/release/?query="
      << UrlEncode(curl, q.str())
      << "&fmt=json&inc=artist-credits+labels+recordings"
      << "&limit=" << limit
      << "&offset=" << offset;
  return url.str();
}

std::string BuildDiscogsUrl(CURL* curl, const LookupQuery& query, size_t limit, size_t page,
                            SearchMode mode) {
  std::ostringstream url;
  url << "https://api.discogs.com/database/search?type=release"
      << "&per_page=" << limit
      << "&page=" << page;

  auto appendParam = [&](const char* key, const std::string& value, bool quoted) {
    if (value.empty()) {
      return;
    }
    std::string v = value;
    if (quoted && mode == SearchMode::ExactPhrase) {
      v = "\"" + v + "\"";
    }
    url << "&" << key << "=" << UrlEncode(curl, v);
  };

  appendParam("artist", query.artist, true);
  appendParam("release_title", query.album, true);
  appendParam("track", query.title, true);
  appendParam("label", query.label, true);
  appendParam("year", query.year, false);

  return url.str();
}

std::string ExtractPrimaryArtist(const nlohmann::json& release) {
  if (release.contains("artist-credit") && release["artist-credit"].is_array() &&
      !release["artist-credit"].empty()) {
    return release["artist-credit"][0].value("name", "");
  }
  return "";
}

int ReadScore(const nlohmann::json& release) {
  if (!release.contains("score")) {
    return 0;
  }
  if (release["score"].is_number_integer()) {
    return release["score"].get<int>();
  }
  if (release["score"].is_string()) {
    try {
      return std::stoi(release["score"].get<std::string>());
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

std::string ExtractPrimaryLabel(const nlohmann::json& release) {
  if (!release.contains("label-info") || !release["label-info"].is_array() ||
      release["label-info"].empty()) {
    return "";
  }

  const auto& li = release["label-info"][0];
  if (!li.contains("label") || !li["label"].is_object()) {
    return "";
  }

  return li["label"].value("name", "");
}

std::string Trim(std::string s) {
  const size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

std::string StripDiscogsLabelSuffix(const std::string& raw) {
  const size_t sep = raw.find(" - ");
  if (sep == std::string::npos) {
    return Trim(raw);
  }
  return Trim(raw.substr(0, sep));
}

std::string JsonToString(const nlohmann::json& v) {
  if (v.is_string()) {
    return v.get<std::string>();
  }
  if (v.is_number_integer()) {
    return std::to_string(v.get<long long>());
  }
  if (v.is_number_unsigned()) {
    return std::to_string(v.get<unsigned long long>());
  }
  if (v.is_number_float()) {
    return std::to_string(v.get<double>());
  }
  return "";
}

std::string JsonFieldToString(const nlohmann::json& obj, const char* key) {
  if (!obj.contains(key)) {
    return "";
  }
  return JsonToString(obj[key]);
}

std::string FindMatchingTrackTitle(const nlohmann::json& release, const std::string& wantedTitle) {
  if (wantedTitle.empty()) {
    return "";
  }

  if (!release.contains("media") || !release["media"].is_array()) {
    return "";
  }

  for (const auto& media : release["media"]) {
    if (!media.contains("tracks") || !media["tracks"].is_array()) {
      continue;
    }
    for (const auto& track : media["tracks"]) {
      const std::string title = track.value("title", "");
      if (title.empty()) {
        continue;
      }
      if (FuzzyContainsNormalized(title, wantedTitle)) {
        return title;
      }
    }
  }

  return "";
}

std::string ToLower(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) {
    return true;
  }
  const std::string h = ToLower(haystack);
  const std::string n = ToLower(needle);
  return h.find(n) != std::string::npos;
}

std::string NormalizeForMatch(const std::string& value) {
  std::string out;
  out.reserve(value.size());

  for (unsigned char c : value) {
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(std::tolower(c)));
    }
  }

  return out;
}

bool FuzzyContainsNormalized(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) {
    return true;
  }

  const std::string h = NormalizeForMatch(haystack);
  const std::string n = NormalizeForMatch(needle);
  if (n.empty()) {
    return true;
  }
  return h.find(n) != std::string::npos;
}

bool MatchesFilledFields(const LookupQuery& query, const TagResult& item) {
  if (!query.artist.empty() && !FuzzyContainsNormalized(item.artist, query.artist)) {
    return false;
  }
  if (!query.title.empty() && !FuzzyContainsNormalized(item.title, query.title)) {
    return false;
  }
  if (!query.album.empty() && !FuzzyContainsNormalized(item.album, query.album)) {
    return false;
  }
  if (!query.label.empty() && !FuzzyContainsNormalized(item.label, query.label)) {
    return false;
  }
  if (!query.year.empty() && !ContainsCaseInsensitive(item.date, query.year)) {
    return false;
  }
  return true;
}

}  // namespace

std::vector<TagResult> TagLookupService::LookupAll(const LookupQuery& query, size_t limit) const {
  std::vector<TagResult> out;

  try {
    if (query.artist.empty() && query.album.empty() && query.label.empty() &&
        query.title.empty() && query.year.empty()) {
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
    if (safe_limit > 50) {
      safe_limit = 50;
    }

    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "foo_taglookup/0.1 (+https://github.com/yourname/foo_taglookup)");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    std::unordered_set<std::string> seen;

  auto collectMusicBrainz = [&](SearchMode mode) {
    const size_t page_size = 100;
    size_t offset = 0;
    size_t total_available = static_cast<size_t>(-1);
    size_t page_guard = 0;

    while ((offset < total_available || total_available == static_cast<size_t>(-1)) &&
           out.size() < safe_limit) {
      std::string response;
      const std::string url = BuildMusicBrainzUrl(curl, query, page_size, offset, mode);

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

      if (json.contains("release-count")) {
        if (json["release-count"].is_number_unsigned()) {
          total_available = json["release-count"].get<size_t>();
        } else if (json["release-count"].is_number_integer()) {
          const auto count = json["release-count"].get<long long>();
          if (count >= 0) {
            total_available = static_cast<size_t>(count);
          }
        } else if (json["release-count"].is_string()) {
          try {
            total_available = static_cast<size_t>(std::stoull(json["release-count"].get<std::string>()));
          } catch (...) {
            // Keep unknown total and rely on page size termination.
          }
        }
      }

      if (!json.contains("releases") || !json["releases"].is_array() || json["releases"].empty()) {
        break;
      }

      const auto& releases = json["releases"];
      const size_t fetched_count = releases.size();

      for (const auto& release : releases) {
        TagResult item;
        item.album = release.value("title", "");
        item.date = release.value("date", "");
        item.release_id = release.value("id", "");
        item.artist = ExtractPrimaryArtist(release);
        item.label = ExtractPrimaryLabel(release);
        item.score = ReadScore(release);
        item.title = query.title;

        if (!query.title.empty()) {
          item.title = FindMatchingTrackTitle(release, query.title);
        }

        if (item.artist.empty() || item.album.empty()) {
          continue;
        }

        if (!MatchesFilledFields(query, item)) {
          continue;
        }

        const std::string key = item.release_id;
        if (!key.empty() && seen.insert(key).second) {
          out.push_back(std::move(item));
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

  auto collectDiscogs = [&](SearchMode mode) {
    const size_t page_size = safe_limit;
    size_t page = 1;
    size_t total_pages = 1;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    const char* discogsToken = std::getenv("DISCOGS_TOKEN");
    if (discogsToken != nullptr && discogsToken[0] != '\0') {
      std::string auth = std::string("Authorization: Discogs token=") + discogsToken;
      headers = curl_slist_append(headers, auth.c_str());
    }
    if (headers != nullptr) {
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    while (page <= total_pages && out.size() < safe_limit) {
      std::string response;
      const std::string url = BuildDiscogsUrl(curl, query, page_size, page, mode);

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

      if (json.contains("pagination") && json["pagination"].is_object() &&
          json["pagination"].contains("pages")) {
        if (json["pagination"]["pages"].is_number_unsigned()) {
          total_pages = json["pagination"]["pages"].get<size_t>();
        } else if (json["pagination"]["pages"].is_number_integer()) {
          const auto p = json["pagination"]["pages"].get<long long>();
          if (p > 0) {
            total_pages = static_cast<size_t>(p);
          }
        }
      }

      if (!json.contains("results") || !json["results"].is_array()) {
        break;
      }

      for (const auto& result : json["results"]) {
        TagResult item;
        item.release_id = JsonFieldToString(result, "id");
        item.date = JsonFieldToString(result, "year");
        item.score = 0;

        const std::string titleCombined = JsonFieldToString(result, "title");
        const size_t sep = titleCombined.find(" - ");
        if (sep != std::string::npos) {
          item.artist = Trim(titleCombined.substr(0, sep));
          item.album = Trim(titleCombined.substr(sep + 3));
        } else {
          item.album = Trim(titleCombined);
          item.artist = query.artist;
        }

        item.title = query.title;
        if (item.title.empty() && result.contains("tracklist") && result["tracklist"].is_array() &&
            !result["tracklist"].empty()) {
          item.title = result["tracklist"][0].value("title", "");
        }

        if (result.contains("label") && result["label"].is_array() && !result["label"].empty() &&
            result["label"][0].is_string()) {
          item.label = StripDiscogsLabelSuffix(result["label"][0].get<std::string>());
        }

        if (item.artist.empty() || item.album.empty()) {
          continue;
        }
        if (!MatchesFilledFields(query, item)) {
          continue;
        }

        if (!item.release_id.empty() && seen.insert(item.release_id).second) {
          out.push_back(std::move(item));
        }

        if (out.size() >= safe_limit) {
          break;
        }
      }

      ++page;
      if (page > 20) {
        break;
      }
    }

      if (headers != nullptr) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
        curl_slist_free_all(headers);
      }
    };

    if (query.provider == SearchProvider::Discogs) {
      collectDiscogs(query.search_mode);
    } else {
      collectMusicBrainz(query.search_mode);
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
  } catch (...) {
    return {};
  }
}

}  // namespace taglookup
