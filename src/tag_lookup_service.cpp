#include "tag_lookup_service.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <sstream>
#include <stdexcept>

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

std::string BuildMusicBrainzUrl(CURL* curl, const LookupQuery& query) {
  std::ostringstream q;
  q << "recording:\"" << query.title << "\" AND artist:\"" << query.artist << "\"";

  std::ostringstream url;
  url << "https://musicbrainz.org/ws/2/recording/?query="
      << UrlEncode(curl, q.str())
      << "&fmt=json&limit=1";
  return url.str();
}

}  // namespace

std::optional<TagResult> TagLookupService::Lookup(const LookupQuery& query) const {
  if (query.artist.empty() || query.title.empty()) {
    return std::nullopt;
  }

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    return std::nullopt;
  }

  std::string response;
  const std::string url = BuildMusicBrainzUrl(curl, query);

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "foo_taglookup/0.1 (+https://github.com/yourname/foo_taglookup)");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

  const CURLcode res = curl_easy_perform(curl);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK || response.empty()) {
    return std::nullopt;
  }

  nlohmann::json json;
  try {
    json = nlohmann::json::parse(response);
  } catch (...) {
    return std::nullopt;
  }

  if (!json.contains("recordings") || !json["recordings"].is_array() || json["recordings"].empty()) {
    return std::nullopt;
  }

  const auto& recording = json["recordings"][0];
  TagResult result;

  result.title = recording.value("title", "");

  if (recording.contains("artist-credit") && recording["artist-credit"].is_array() &&
      !recording["artist-credit"].empty()) {
    result.artist = recording["artist-credit"][0].value("name", "");
  }

  if (recording.contains("releases") && recording["releases"].is_array() && !recording["releases"].empty()) {
    const auto& rel = recording["releases"][0];
    result.album = rel.value("title", "");
    result.date = rel.value("date", "");
  }

  if (result.artist.empty() || result.title.empty()) {
    return std::nullopt;
  }

  return result;
}

}  // namespace taglookup
