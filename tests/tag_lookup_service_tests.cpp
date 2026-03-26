#include <curl/curl.h>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include "../src/tag_lookup_service.cpp"

namespace {

void Expect(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void TestDiscogsMasterUrlUsesMasterSearchAndOmitsTrackByDefault() {
  CURL* curl = curl_easy_init();
  Expect(curl != nullptr, "curl_easy_init failed");

  taglookup::LookupQuery query;
  query.artist = "Radiohead";
  query.album = "OK Computer";
  query.title = "Paranoid Android";
  query.label = "Parlophone";
  query.year = "1997";

  const std::string url = taglookup::BuildDiscogsUrl(
      curl, query, 5, 2, taglookup::SearchMode::ExactPhrase, false, false);
  curl_easy_cleanup(curl);

  Expect(url.find("type=master") != std::string::npos, "Discogs search should target masters");
  Expect(url.find("page=2") != std::string::npos, "Discogs page parameter missing");
  Expect(url.find("track=") == std::string::npos,
         "Discogs master lookup should omit track when includeTrack=false");
  Expect(url.find("artist=Radiohead") != std::string::npos, "Discogs artist parameter missing");
  Expect(url.find("release_title=OK%20Computer") != std::string::npos,
         "Discogs release_title parameter missing");
}

void TestDiscogsGeneralQueryIncludesAllFilledFields() {
  CURL* curl = curl_easy_init();
  Expect(curl != nullptr, "curl_easy_init failed");

  taglookup::LookupQuery query;
  query.artist = "Radiohead";
  query.album = "OK Computer";
  query.title = "Paranoid Android";
  query.year = "1997";

  const std::string url =
      taglookup::BuildDiscogsUrl(curl, query, 10, 1, taglookup::SearchMode::Tokenized, false, true);
  curl_easy_cleanup(curl);

  Expect(url.find("q=Radiohead%20OK%20Computer%20Paranoid%20Android%201997") !=
             std::string::npos,
         "Discogs q fallback should contain the combined query fields");
}

void TestMatchesFilledFieldsRequiresAllProvidedFields() {
  taglookup::LookupQuery query;
  query.artist = "Radiohead";
  query.title = "Paranoid Android";
  query.album = "OK Computer";
  query.label = "Parlophone";
  query.year = "1997";

  taglookup::TagResult match;
  match.artist = "Radiohead";
  match.title = "Paranoid Android";
  match.album = "OK Computer";
  match.label = "Parlophone Records";
  match.date = "1997-05-26";

  Expect(taglookup::MatchesFilledFields(query, match), "Expected all provided fields to match");

  match.date = "1998";
    Expect(!taglookup::MatchesFilledFields(query, match),
      "Expected year mismatch to fail matching");
}

void TestExtractDiscogsMasterCoverUrlPrefersUri150ForPreviews() {
  const auto json = nlohmann::json::parse(R"json({
    "images": [
      {
        "uri": "https://img.example/full.jpg",
        "uri150": "https://img.example/thumb.jpg"
      }
    ]
  })json");

  Expect(taglookup::ExtractDiscogsMasterCoverUrl(json) == "https://img.example/thumb.jpg",
         "Tag lookup previews should use the smaller Discogs image");
}

}  // namespace

int main() {
  curl_global_init(CURL_GLOBAL_DEFAULT);

  try {
    TestDiscogsMasterUrlUsesMasterSearchAndOmitsTrackByDefault();
    TestDiscogsGeneralQueryIncludesAllFilledFields();
    TestMatchesFilledFieldsRequiresAllProvidedFields();
    TestExtractDiscogsMasterCoverUrlPrefersUri150ForPreviews();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    curl_global_cleanup();
    return EXIT_FAILURE;
  }

  curl_global_cleanup();
  return EXIT_SUCCESS;
}