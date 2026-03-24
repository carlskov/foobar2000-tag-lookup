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

std::string FindFirstTrackTitle(const nlohmann::json& release) {
  if (release.contains("tracklist") && release["tracklist"].is_array()) {
    for (const auto& track : release["tracklist"]) {
      const std::string type = JsonFieldToString(track, "type_");
      const std::string title = JsonFieldToString(track, "title");
      if (!title.empty() && (type.empty() || type == "track" || type == "index")) {
        return title;
      }
    }
  }

  if (release.contains("media") && release["media"].is_array()) {
    for (const auto& media : release["media"]) {
      if (!media.contains("tracks") || !media["tracks"].is_array()) {
        continue;
      }
      for (const auto& track : media["tracks"]) {
        const std::string title = track.value("title", "");
        if (!title.empty()) {
          return title;
        }
      }
    }
  }

  return "";
}

std::string ResolveTrackTitleFromReleaseJson(const nlohmann::json& release,
                                             const std::string& wantedTitle) {
  if (!wantedTitle.empty()) {
    const std::string matchedTitle = FindMatchingTrackTitle(release, wantedTitle);
    if (!matchedTitle.empty()) {
      return matchedTitle;
    }
    return wantedTitle;
  }

  return FindFirstTrackTitle(release);
}

std::string JoinArtistNames(const nlohmann::json& artists) {
  if (!artists.is_array() || artists.empty()) {
    return "";
  }

  std::string joined;
  for (const auto& artist : artists) {
    const std::string name = JsonFieldToString(artist, "name");
    if (name.empty()) {
      continue;
    }

    if (!joined.empty()) {
      const std::string join = JsonFieldToString(artist, "join");
      joined += join.empty() ? ", " : std::string(" ") + join + " ";
    }
    joined += name;
  }

  return Trim(joined);
}

std::string ExtractDiscogsReleaseArtist(const nlohmann::json& release) {
  if (release.contains("artists")) {
    const std::string artist = JoinArtistNames(release["artists"]);
    if (!artist.empty()) {
      return artist;
    }
  }
  return "";
}

std::string ExtractDiscogsMediaType(const nlohmann::json& release) {
  if (!release.contains("formats") || !release["formats"].is_array() ||
      release["formats"].empty()) {
    return "";
  }

  const auto& first = release["formats"][0];
  if (!first.is_object()) {
    return "";
  }

  const std::string name = JsonFieldToString(first, "name");
  if (!name.empty()) {
    return name;
  }

  return "";
}

std::string ExtractDiscNumberFromDiscogsPosition(const std::string& position) {
  // Common Discogs patterns: "1-03", "2.07", "CD1-4". Keep it conservative.
  const std::string p = Trim(position);
  if (p.empty()) {
    return "";
  }

  auto readDigits = [&](size_t from) {
    std::string out;
    for (size_t i = from; i < p.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(p[i]))) {
        break;
      }
      out.push_back(p[i]);
    }
    return out;
  };

  const std::string leading = readDigits(0);
  if (!leading.empty() && leading.size() < p.size()) {
    const char sep = p[leading.size()];
    if (sep == '-' || sep == '.' || sep == ':') {
      return leading;
    }
  }

  if ((p.rfind("CD", 0) == 0 || p.rfind("cd", 0) == 0) && p.size() > 2) {
    const std::string cdNum = readDigits(2);
    if (!cdNum.empty()) {
      return cdNum;
    }
  }

  return "";
}

const nlohmann::json* FindMatchingDiscogsTrack(const nlohmann::json& release,
                                               const std::string& wantedTitle) {
  if (!release.contains("tracklist") || !release["tracklist"].is_array()) {
    return nullptr;
  }

  const nlohmann::json* firstTrack = nullptr;
  for (const auto& track : release["tracklist"]) {
    const std::string type = JsonFieldToString(track, "type_");
    if (!(type.empty() || type == "track" || type == "index")) {
      continue;
    }

    const std::string title = JsonFieldToString(track, "title");
    if (title.empty()) {
      continue;
    }

    if (firstTrack == nullptr) {
      firstTrack = &track;
    }

    if (!wantedTitle.empty() && FuzzyContainsNormalized(title, wantedTitle)) {
      return &track;
    }
  }

  return firstTrack;
}

std::string ResolveDiscogsTrackArtistFromReleaseJson(const nlohmann::json& release,
                                                     const std::string& wantedTitle,
                                                     const std::string& fallbackArtist) {
  const nlohmann::json* track = FindMatchingDiscogsTrack(release, wantedTitle);
  if (track != nullptr) {
    if (track->contains("artists")) {
      const std::string trackArtist = JoinArtistNames((*track)["artists"]);
      if (!trackArtist.empty()) {
        return trackArtist;
      }
    }
  }

  const std::string releaseArtist = ExtractDiscogsReleaseArtist(release);
  if (!releaseArtist.empty()) {
    return releaseArtist;
  }

  return fallbackArtist;
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
      const std::string url = BuildMusicBrainzUrl(curl, query, page_size, offset, mode);
      nlohmann::json json;
      if (!PerformJsonRequest(curl, url, json)) {
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
          const std::string matchedTitle = FindMatchingTrackTitle(release, query.title);
          if (!matchedTitle.empty()) {
            item.title = matchedTitle;
          }
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
      const std::string url = BuildDiscogsUrl(curl, query, page_size, page, mode);
      nlohmann::json json;
      if (!PerformJsonRequest(curl, url, json, headers)) {
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

TracklistResult TagLookupService::FetchTracklist(const LookupQuery& query,
                                                 const TagResult& result) const {
  TracklistResult out;

  try {
    if (result.release_id.empty()) {
      return out;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
      return out;
    }

    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "foo_taglookup/0.1 (+https://github.com/yourname/foo_taglookup)");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");

    std::string url;

    if (query.provider == SearchProvider::Discogs) {
      const char* discogsToken = std::getenv("DISCOGS_TOKEN");
      if (discogsToken != nullptr && discogsToken[0] != '\0') {
        std::string auth = std::string("Authorization: Discogs token=") + discogsToken;
        headers = curl_slist_append(headers, auth.c_str());
      }
      url = "https://api.discogs.com/releases/" + result.release_id;
    } else {
      // MusicBrainz: fetch release by ID with recordings and artist credits.
      url = "https://musicbrainz.org/ws/2/release/" + result.release_id +
            "?inc=recordings+artist-credits&fmt=json";
    }

    nlohmann::json json;
    const bool ok = PerformJsonRequest(curl, url, json, headers);

    if (headers != nullptr) {
      curl_slist_free_all(headers);
    }
    curl_easy_cleanup(curl);

    if (!ok) {
      return out;
    }

    if (query.provider == SearchProvider::Discogs) {
      // Album-level artist.
      out.albumArtist = ExtractDiscogsReleaseArtist(json);

      // Per-track data from tracklist[].
      const std::string releaseMediaType = ExtractDiscogsMediaType(json);
      size_t trackOrdinal = 1;
      if (json.contains("tracklist") && json["tracklist"].is_array()) {
        for (const auto& track : json["tracklist"]) {
          const std::string type = JsonFieldToString(track, "type_");
          if (!(type.empty() || type == "track" || type == "index")) {
            continue;
          }
          const std::string title = JsonFieldToString(track, "title");
          if (title.empty()) {
            continue;
          }
          TrackInfo info;
          info.title = title;
          if (track.contains("artists")) {
            info.artist = JoinArtistNames(track["artists"]);
          }
          info.trackNumber = JsonFieldToString(track, "position");
          if (info.trackNumber.empty()) {
            info.trackNumber = std::to_string(trackOrdinal);
          }
          info.discNumber = ExtractDiscNumberFromDiscogsPosition(info.trackNumber);
          info.mediaType = releaseMediaType;
          out.tracks.push_back(std::move(info));
          ++trackOrdinal;
        }
      }
    } else {
      // MusicBrainz album-level artist.
      out.albumArtist = ExtractPrimaryArtist(json);

      // Per-track data from media[].tracks[].
      if (json.contains("media") && json["media"].is_array()) {
        size_t mediumOrdinal = 1;
        for (const auto& medium : json["media"]) {
          const std::string mediaType = medium.value("format", "");
          std::string discNumber;
          if (medium.contains("position") && medium["position"].is_number_integer()) {
            discNumber = std::to_string(medium["position"].get<long long>());
          } else {
            discNumber = std::to_string(mediumOrdinal);
          }
          if (!medium.contains("tracks") || !medium["tracks"].is_array()) {
            ++mediumOrdinal;
            continue;
          }
          size_t trackOrdinal = 1;
          for (const auto& track : medium["tracks"]) {
            const std::string title = track.value("title", "");
            if (title.empty()) {
              continue;
            }
            TrackInfo info;
            info.title = title;
            info.trackNumber = track.value("number", "");
            if (info.trackNumber.empty()) {
              if (track.contains("position") && track["position"].is_number_integer()) {
                info.trackNumber =
                    std::to_string(track["position"].get<long long>());
              } else {
                info.trackNumber = std::to_string(trackOrdinal);
              }
            }
            info.discNumber = discNumber;
            info.mediaType = mediaType;
            // Per-track artist (if different from album artist).
            if (track.contains("artist-credit") && track["artist-credit"].is_array() &&
                !track["artist-credit"].empty()) {
              info.artist = track["artist-credit"][0].value("name", "");
            }
            out.tracks.push_back(std::move(info));
            ++trackOrdinal;
          }
          ++mediumOrdinal;
        }
      }
    }
  } catch (...) {
  }

  return out;
}

std::string TagLookupService::ResolvePropagationTitle(const LookupQuery& query,
                                                     const TagResult& result) const {
  if (!result.title.empty()) {
    return result.title;
  }

  try {
    if (query.provider == SearchProvider::Discogs && !result.release_id.empty()) {
      CURL* curl = curl_easy_init();
      if (curl == nullptr) {
        return query.title;
      }

      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(curl, CURLOPT_USERAGENT,
                       "foo_taglookup/0.1 (+https://github.com/yourname/foo_taglookup)");
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

      struct curl_slist* headers = nullptr;
      headers = curl_slist_append(headers, "Accept: application/json");
      const char* discogsToken = std::getenv("DISCOGS_TOKEN");
      if (discogsToken != nullptr && discogsToken[0] != '\0') {
        std::string auth = std::string("Authorization: Discogs token=") + discogsToken;
        headers = curl_slist_append(headers, auth.c_str());
      }

      const std::string url = "https://api.discogs.com/releases/" + result.release_id;
      nlohmann::json json;
      const bool ok = PerformJsonRequest(curl, url, json, headers);

      if (headers != nullptr) {
        curl_slist_free_all(headers);
      }
      curl_easy_cleanup(curl);

      if (ok) {
        const std::string resolved = ResolveTrackTitleFromReleaseJson(json, query.title);
        if (!resolved.empty()) {
          return resolved;
        }
      }
    }
  } catch (...) {
  }

  return query.title;
}

std::string TagLookupService::ResolvePropagationArtist(const LookupQuery& query,
                                                      const TagResult& result) const {
  try {
    if (query.provider == SearchProvider::Discogs && !result.release_id.empty()) {
      CURL* curl = curl_easy_init();
      if (curl == nullptr) {
        return !query.artist.empty() ? query.artist : result.artist;
      }

      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(curl, CURLOPT_USERAGENT,
                       "foo_taglookup/0.1 (+https://github.com/yourname/foo_taglookup)");
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

      struct curl_slist* headers = nullptr;
      headers = curl_slist_append(headers, "Accept: application/json");
      const char* discogsToken = std::getenv("DISCOGS_TOKEN");
      if (discogsToken != nullptr && discogsToken[0] != '\0') {
        std::string auth = std::string("Authorization: Discogs token=") + discogsToken;
        headers = curl_slist_append(headers, auth.c_str());
      }

      const std::string url = "https://api.discogs.com/releases/" + result.release_id;
      nlohmann::json json;
      const bool ok = PerformJsonRequest(curl, url, json, headers);

      if (headers != nullptr) {
        curl_slist_free_all(headers);
      }
      curl_easy_cleanup(curl);

      if (ok) {
        const std::string fallbackArtist = !query.artist.empty() ? query.artist : result.artist;
        const std::string resolved =
            ResolveDiscogsTrackArtistFromReleaseJson(json, query.title, fallbackArtist);
        if (!resolved.empty()) {
          return resolved;
        }
      }
    }
  } catch (...) {
  }

  if (!query.artist.empty()) {
    return query.artist;
  }
  return result.artist;
}

}  // namespace taglookup
