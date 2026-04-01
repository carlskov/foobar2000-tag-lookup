#include <curl/curl.h>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include "../src/album_art_service.cpp"

namespace {

void Expect(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void TestDiscogsAlbumArtSearchUsesMasters() {
  CURL* curl = curl_easy_init();
  Expect(curl != nullptr, "curl_easy_init failed");

  taglookup::AlbumArtQuery query;
  query.artist = "Radiohead";
  query.album = "OK Computer";
  query.title = "Paranoid Android";

  const std::string url = taglookup::BuildDiscogsSearchUrl(
      curl, query, 10, taglookup::SearchMode::ExactPhrase, false, true);
  curl_easy_cleanup(curl);

  Expect(url.find("type=master") != std::string::npos,
         "Discogs album art search should target masters");
  Expect(url.find("q=Radiohead%20OK%20Computer%20Paranoid%20Android") != std::string::npos,
         "Discogs album art fallback query missing expected fields");
}

void TestGuessExtensionNormalizesJpegToJpg() {
  Expect(taglookup::AlbumArtService::GuessExtension("https://img.example/cover.jpeg", "", "") ==
             ".jpg",
         "JPEG URLs should normalize to .jpg");
  Expect(taglookup::AlbumArtService::GuessExtension("https://img.example/cover", "image/jpeg", "") ==
             ".jpg",
         "JPEG content types should normalize to .jpg");
}

void TestExtractDiscogsMasterCoverUrlPrefersFullSizeImage() {
  const auto json = nlohmann::json::parse(R"json({
    "images": [
      {
        "uri": "https://img.example/full.jpg",
        "uri150": "https://img.example/thumb.jpg"
      }
    ]
  })json");

  Expect(taglookup::ExtractDiscogsMasterCoverUrl(json) == "https://img.example/full.jpg",
         "Album art downloads should prefer the full-size Discogs image");
}

void TestLooksLikeImageBytesRecognizesCommonFormats() {
  const std::string jpegBytes = std::string("\xFF\xD8\xFF", 3) + "rest";
  const std::string pngBytes = std::string("\x89PNG\r\n\x1A\n", 8) + "rest";
  const std::string webpBytes = "RIFFxxxxWEBPrest";

  Expect(taglookup::LooksLikeImageBytes(jpegBytes), "JPEG signature should be recognized");
  Expect(taglookup::LooksLikeImageBytes(pngBytes), "PNG signature should be recognized");
  Expect(taglookup::LooksLikeImageBytes(webpBytes), "WEBP signature should be recognized");
  Expect(!taglookup::LooksLikeImageBytes("<html>not an image</html>"),
         "HTML should not be treated as image data");
}

// ---------------------------------------------------------------------------
// JsonToString (album_art_service)
// ---------------------------------------------------------------------------

void TestAlbumArtJsonToStringConvertsVariousTypes() {
  auto jsonString = nlohmann::json::parse(R"json("hello")json");
  Expect(taglookup::JsonToString(jsonString) == "hello",
         "JsonToString should return string value");

  auto jsonInt = nlohmann::json::parse(R"json(42)json");
  Expect(taglookup::JsonToString(jsonInt) == "42",
         "JsonToString should convert integer to string");

  nlohmann::json jsonUnsigned = 4294967295U;
  Expect(taglookup::JsonToString(jsonUnsigned) == "4294967295",
         "JsonToString should convert unsigned integer to string");

  nlohmann::json jsonDouble = 3.0;
  Expect(taglookup::JsonToString(jsonDouble) == "3.000000",
         "JsonToString should convert double to string");

  auto jsonNull = nlohmann::json::parse(R"json(null)json");
  Expect(taglookup::JsonToString(jsonNull) == "",
         "JsonToString should return empty string for null");
}

// ---------------------------------------------------------------------------
// JsonFieldToString (album_art_service)
// ---------------------------------------------------------------------------

void TestAlbumArtJsonFieldToStringReturnsFieldValue() {
  const auto json = nlohmann::json::parse(R"json({ "name": "Test", "count": 5 })json");
  Expect(taglookup::JsonFieldToString(json, "name") == "Test",
         "JsonFieldToString should return string field value");
  Expect(taglookup::JsonFieldToString(json, "count") == "5",
         "JsonFieldToString should convert numeric field to string");
  Expect(taglookup::JsonFieldToString(json, "missing") == "",
         "JsonFieldToString should return empty for missing field");
}

// ---------------------------------------------------------------------------
// ToLower (album_art_service)
// ---------------------------------------------------------------------------

void TestAlbumArtToLowerConvertsCharacters() {
  Expect(taglookup::ToLower("HELLO WORLD") == "hello world",
         "ToLower should convert uppercase to lowercase");
  Expect(taglookup::ToLower("Already Lower") == "already lower",
         "ToLower should leave lowercase unchanged");
  Expect(taglookup::ToLower("") == "",
         "ToLower should handle empty string");
  Expect(taglookup::ToLower("MiXeD CaSe") == "mixed case",
         "ToLower should handle mixed case");
}

// ---------------------------------------------------------------------------
// ExtensionFromUrl
// ---------------------------------------------------------------------------

void TestExtensionFromUrlExtractsExtension() {
  Expect(taglookup::ExtensionFromUrl("https://example.com/cover.jpg") == ".jpg",
         "ExtensionFromUrl should extract .jpg");
  Expect(taglookup::ExtensionFromUrl("https://example.com/cover.JPEG") == ".jpg",
         "ExtensionFromUrl should normalize .JPEG to .jpg");
  Expect(taglookup::ExtensionFromUrl("https://example.com/cover.png") == ".png",
         "ExtensionFromUrl should extract .png");
  Expect(taglookup::ExtensionFromUrl("https://example.com/cover.webp") == ".webp",
         "ExtensionFromUrl should extract .webp");
  Expect(taglookup::ExtensionFromUrl("https://example.com/cover") == "",
         "ExtensionFromUrl should return empty for no extension");
  Expect(taglookup::ExtensionFromUrl("https://example.com/cover?size=large") == "",
         "ExtensionFromUrl should ignore query string");
  Expect(taglookup::ExtensionFromUrl("https://example.com/cover.txt") == "",
         "ExtensionFromUrl should return empty for unsupported extension");
}

// ---------------------------------------------------------------------------
// JoinJsonStringArray (album_art_service)
// ---------------------------------------------------------------------------

void TestAlbumArtJoinJsonStringArrayJoinsWithSemicolon() {
  const auto json = nlohmann::json::parse(R"json(["Rock", "Alternative", "Indie"])json");
  Expect(taglookup::JoinJsonStringArray(json) == "Rock; Alternative; Indie",
         "JoinJsonStringArray should join with '; '");
}

void TestAlbumArtJoinJsonStringArraySkipsNonStrings() {
  const auto json = nlohmann::json::parse(R"json(["Rock", 42, 3.5, "Indie"])json");
  Expect(taglookup::JoinJsonStringArray(json) == "Rock; 42; 3.500000; Indie",
         "JoinJsonStringArray should convert numbers to string");
}

void TestAlbumArtJoinJsonStringArrayReturnsEmptyForNonArray() {
  const auto json = nlohmann::json::parse(R"json("not an array")json");
  Expect(taglookup::JoinJsonStringArray(json) == "",
         "JoinJsonStringArray should return empty for non-array");
  Expect(taglookup::JoinJsonStringArray(nlohmann::json::array()) == "",
         "JoinJsonStringArray should return empty for empty array");
}

// ---------------------------------------------------------------------------
// JoinArtistNames (album_art_service)
// ---------------------------------------------------------------------------

void TestAlbumArtJoinArtistNamesConcatenatesWithComma() {
  const auto json = nlohmann::json::parse(R"json([
    { "name": "Artist A" },
    { "name": "Artist B" }
  ])json");
  Expect(taglookup::JoinArtistNames(json) == "Artist A, Artist B",
         "JoinArtistNames should join multiple artists with ', '");
}

void TestAlbumArtJoinArtistNamesReturnsEmptyForNonArray() {
  const auto json = nlohmann::json::parse(R"json("not artists")json");
  Expect(taglookup::JoinArtistNames(json) == "",
         "JoinArtistNames should return empty for non-array");
  Expect(taglookup::JoinArtistNames(nlohmann::json::array()) == "",
         "JoinArtistNames should return empty for empty array");
}

void TestAlbumArtJoinArtistNamesSkipsMissingNames() {
  const auto json = nlohmann::json::parse(R"json([
    { "name": "Artist A" },
    { "other": "field" }
  ])json");
  Expect(taglookup::JoinArtistNames(json) == "Artist A",
         "JoinArtistNames should skip entries without name field");
}

// ---------------------------------------------------------------------------
// BuildMusicBrainzUrl
// ---------------------------------------------------------------------------

void TestBuildMusicBrainzUrlIncludesRequiredParameters() {
  CURL* curl = curl_easy_init();
  Expect(curl != nullptr, "curl_easy_init failed");

  taglookup::AlbumArtQuery query;
  query.artist = "Radiohead";
  query.album = "OK Computer";
  query.title = "Paranoid Android";
  query.label = "Parlophone";
  query.year = "1997";

  const std::string url = taglookup::BuildMusicBrainzUrl(
      curl, query, 10, taglookup::SearchMode::ExactPhrase);
  curl_easy_cleanup(curl);

  Expect(url.find("musicbrainz.org") != std::string::npos,
         "URL should target MusicBrainz");
  Expect(url.find("fmt=json") != std::string::npos,
         "URL should request JSON format");
  Expect(url.find("inc=cover-art-archive") != std::string::npos,
         "URL should include cover-art-archive");
  Expect(url.find("limit=10") != std::string::npos,
         "URL should include limit parameter");
}

void TestBuildMusicBrainzUrlTokenizedModeOmitsQuotes() {
  CURL* curl = curl_easy_init();
  Expect(curl != nullptr, "curl_easy_init failed");

  taglookup::AlbumArtQuery query;
  query.artist = "Radiohead";
  query.album = "OK Computer";

  const std::string exactUrl = taglookup::BuildMusicBrainzUrl(
      curl, query, 10, taglookup::SearchMode::ExactPhrase);
  const std::string tokenizedUrl = taglookup::BuildMusicBrainzUrl(
      curl, query, 10, taglookup::SearchMode::Tokenized);
  curl_easy_cleanup(curl);

  Expect(exactUrl.find("%22") != std::string::npos,
         "ExactPhrase URL should URL-encode quotes");
  Expect(tokenizedUrl.find("%22") == std::string::npos,
         "Tokenized URL should omit quotes");
}

void TestBuildMusicBrainzUrlHandlesEmptyQuery() {
  CURL* curl = curl_easy_init();
  Expect(curl != nullptr, "curl_easy_init failed");

  taglookup::AlbumArtQuery query;

  const std::string url = taglookup::BuildMusicBrainzUrl(
      curl, query, 10, taglookup::SearchMode::ExactPhrase);
  curl_easy_cleanup(curl);

  Expect(url.find("release%3A%22%22") != std::string::npos,
         "Empty query should include empty release clause (URL-encoded)");
}

// ---------------------------------------------------------------------------
// BuildDiscogsSearchUrl - additional tests
// ---------------------------------------------------------------------------

void TestBuildDiscogsSearchUrlIncludesTrackWhenRequested() {
  CURL* curl = curl_easy_init();
  Expect(curl != nullptr, "curl_easy_init failed");

  taglookup::AlbumArtQuery query;
  query.artist = "Radiohead";
  query.album = "OK Computer";
  query.title = "Paranoid Android";

  const std::string urlWithTrack = taglookup::BuildDiscogsSearchUrl(
      curl, query, 10, taglookup::SearchMode::ExactPhrase, true, false);
  const std::string urlWithoutTrack = taglookup::BuildDiscogsSearchUrl(
      curl, query, 10, taglookup::SearchMode::ExactPhrase, false, false);
  curl_easy_cleanup(curl);

  Expect(urlWithTrack.find("track=") != std::string::npos,
         "URL should include track parameter when includeTrack=true");
  Expect(urlWithoutTrack.find("track=") == std::string::npos,
         "URL should omit track parameter when includeTrack=false");
}

// ---------------------------------------------------------------------------
// GuessExtension - additional tests
// ---------------------------------------------------------------------------

void TestGuessExtensionUsesExtensionHint() {
  Expect(taglookup::AlbumArtService::GuessExtension("url", "", ".png") == ".png",
         "GuessExtension should return hint when provided");
}

void TestGuessExtensionDerivesFromContentType() {
  Expect(taglookup::AlbumArtService::GuessExtension("url", "image/png", "") == ".png",
         "GuessExtension should derive .png from content type");
  Expect(taglookup::AlbumArtService::GuessExtension("url", "image/webp", "") == ".webp",
         "GuessExtension should derive .webp from content type");
  Expect(taglookup::AlbumArtService::GuessExtension("url", "application/octet-stream", "") == ".jpg",
         "GuessExtension should default to .jpg for unknown content type");
}

void TestGuessExtensionDerivesFromUrl() {
  Expect(taglookup::AlbumArtService::GuessExtension("cover.png", "", "") == ".png",
         "GuessExtension should derive extension from URL");
}

// ---------------------------------------------------------------------------
// BuildAlbumArtExchangeSearchUrl
// ---------------------------------------------------------------------------

void TestBuildAlbumArtExchangeSearchUrlIncludesArtistAndAlbum() {
  CURL* curl = curl_easy_init();
  Expect(curl != nullptr, "curl_easy_init failed");

  taglookup::AlbumArtQuery query;
  query.artist = "Radiohead";
  query.album = "OK Computer";

  const std::string url = taglookup::BuildAlbumArtExchangeSearchUrl(curl, query, 1);
  curl_easy_cleanup(curl);

  Expect(url.find("albumartexchange.com") != std::string::npos,
         "URL should target AlbumArtExchange");
  Expect(url.find("q=Radiohead%20OK%20Computer") != std::string::npos,
         "URL should include artist and album query");
  Expect(url.find("fltr=ARTISTTITLE") != std::string::npos,
         "URL should include ARTISTTITLE filter");
  Expect(url.find("page=1") != std::string::npos,
         "URL should include page parameter");
}

void TestBuildAlbumArtExchangeSearchUrlHandlesMultipleFields() {
  CURL* curl = curl_easy_init();
  Expect(curl != nullptr, "curl_easy_init failed");

  taglookup::AlbumArtQuery query;
  query.artist = "Radiohead";
  query.album = "OK Computer";
  query.title = "Paranoid Android";
  query.label = "Parlophone";
  query.year = "1997";

  const std::string url = taglookup::BuildAlbumArtExchangeSearchUrl(curl, query, 2);
  curl_easy_cleanup(curl);

  Expect(url.find("Radiohead") != std::string::npos,
         "URL should include artist");
  Expect(url.find("OK%20Computer") != std::string::npos,
         "URL should include album");
  Expect(url.find("Paranoid%20Android") != std::string::npos,
         "URL should include title");
  Expect(url.find("page=2") != std::string::npos,
         "URL should include page 2");
}

void TestBuildAlbumArtExchangeSearchUrlHandlesEmptyFields() {
  CURL* curl = curl_easy_init();
  Expect(curl != nullptr, "curl_easy_init failed");

  taglookup::AlbumArtQuery query;
  query.artist = "Radiohead";

  const std::string url = taglookup::BuildAlbumArtExchangeSearchUrl(curl, query, 1);
  curl_easy_cleanup(curl);

  Expect(url.find("q=Radiohead") != std::string::npos,
         "URL should include only the non-empty artist field");
}

// ---------------------------------------------------------------------------
// ExtractCoverIdFromHtml
// ---------------------------------------------------------------------------

void TestExtractCoverIdFromHtmlFindsDataCoverId() {
  const std::string html = R"html(
    <div class="cover-item">
      <a href="/covers/12345-slug-name">Cover</a>
    </div>
  )html";
  size_t pos = 0;
  std::string coverId = taglookup::ExtractCoverIdFromHtml(html, pos);
  Expect(coverId == "12345",
         "ExtractCoverIdFromHtml should find cover ID");
}

void TestExtractCoverIdFromHtmlReturnsEmptyWhenNotFound() {
  const std::string html = "<div>No cover IDs here</div>";
  size_t pos = 0;
  std::string coverId = taglookup::ExtractCoverIdFromHtml(html, pos);
  Expect(coverId.empty(),
         "ExtractCoverIdFromHtml should return empty when no cover ID found");
}

void TestExtractCoverIdFromHtmlAdvancesPosition() {
  const std::string html = "data-coverid=\"abc\" more data-coverid=\"xyz\"";
  size_t pos = 0;
  std::string firstId = taglookup::ExtractCoverIdFromHtml(html, pos);
  Expect(firstId == "abc",
         "First cover ID should be 'abc'");
  std::string secondId = taglookup::ExtractCoverIdFromHtml(html, pos);
  Expect(secondId == "xyz",
         "Second call should find 'xyz' (position advanced)");
}

// ---------------------------------------------------------------------------
// ExtractSlugFromDetailLink
// ---------------------------------------------------------------------------

void TestExtractSlugFromDetailLinkExtractsSlug() {
  Expect(taglookup::ExtractSlugFromDetailLink("/covers/123-slug-name") == "slug-name",
         "Should extract slug after dash from detail link");
  Expect(taglookup::ExtractSlugFromDetailLink("/covers/456-another-slug") == "another-slug",
         "Should handle longer slugs");
}

void TestExtractSlugFromDetailLinkReturnsEmptyForInvalidLinks() {
  Expect(taglookup::ExtractSlugFromDetailLink("").empty(),
         "Should return empty for empty string");
  Expect(taglookup::ExtractSlugFromDetailLink("/no-dash-here").empty(),
         "Should return empty for links without dash separator");
  Expect(taglookup::ExtractSlugFromDetailLink("not-a-path").empty(),
         "Should return empty for paths without slash");
}

// ---------------------------------------------------------------------------
// BuildDiscogsSearchUrl with discogsType parameter
// ---------------------------------------------------------------------------

void TestBuildDiscogsSearchUrlUsesMasterTypeByDefault() {
  CURL* curl = curl_easy_init();
  Expect(curl != nullptr, "curl_easy_init failed");

  taglookup::AlbumArtQuery query;
  query.artist = "Radiohead";
  query.album = "OK Computer";

  const std::string url = taglookup::BuildDiscogsSearchUrl(
      curl, query, 10, taglookup::SearchMode::ExactPhrase, false, false);
  curl_easy_cleanup(curl);

  Expect(url.find("type=master") != std::string::npos,
         "Default Discogs search should use type=master");
}

void TestBuildDiscogsSearchUrlAcceptsReleaseType() {
  CURL* curl = curl_easy_init();
  Expect(curl != nullptr, "curl_easy_init failed");

  taglookup::AlbumArtQuery query;
  query.artist = "Radiohead";
  query.album = "OK Computer";

  const std::string url = taglookup::BuildDiscogsSearchUrl(
      curl, query, 10, taglookup::SearchMode::ExactPhrase, false, false, "release");
  curl_easy_cleanup(curl);

  Expect(url.find("type=release") != std::string::npos,
         "Discogs search with discogsType='release' should use type=release");
}

void TestBuildDiscogsSearchUrlWithReleaseTypeIncludesAllFields() {
  CURL* curl = curl_easy_init();
  Expect(curl != nullptr, "curl_easy_init failed");

  taglookup::AlbumArtQuery query;
  query.artist = "Radiohead";
  query.album = "OK Computer";
  query.label = "Parlophone";
  query.year = "1997";

  const std::string url = taglookup::BuildDiscogsSearchUrl(
      curl, query, 10, taglookup::SearchMode::ExactPhrase, false, false, "release");
  curl_easy_cleanup(curl);

  Expect(url.find("artist=Radiohead") != std::string::npos,
         "URL should include artist");
  Expect(url.find("release_title=OK%20Computer") != std::string::npos,
         "URL should include album as release_title");
  Expect(url.find("label=Parlophone") != std::string::npos,
         "URL should include label");
  Expect(url.find("year=1997") != std::string::npos,
         "URL should include year");
}

}  // namespace

int main() {
  curl_global_init(CURL_GLOBAL_DEFAULT);

  try {
    TestDiscogsAlbumArtSearchUsesMasters();
    TestGuessExtensionNormalizesJpegToJpg();
    TestExtractDiscogsMasterCoverUrlPrefersFullSizeImage();
    TestLooksLikeImageBytesRecognizesCommonFormats();

    TestAlbumArtJsonToStringConvertsVariousTypes();
    TestAlbumArtJsonFieldToStringReturnsFieldValue();
    TestAlbumArtToLowerConvertsCharacters();
    TestExtensionFromUrlExtractsExtension();
    TestAlbumArtJoinJsonStringArrayJoinsWithSemicolon();
    TestAlbumArtJoinJsonStringArraySkipsNonStrings();
    TestAlbumArtJoinJsonStringArrayReturnsEmptyForNonArray();
    TestAlbumArtJoinArtistNamesConcatenatesWithComma();
    TestAlbumArtJoinArtistNamesReturnsEmptyForNonArray();
    TestAlbumArtJoinArtistNamesSkipsMissingNames();
    TestBuildMusicBrainzUrlIncludesRequiredParameters();
    TestBuildMusicBrainzUrlTokenizedModeOmitsQuotes();
    TestBuildMusicBrainzUrlHandlesEmptyQuery();
    TestBuildDiscogsSearchUrlIncludesTrackWhenRequested();
    TestGuessExtensionUsesExtensionHint();
    TestGuessExtensionDerivesFromContentType();
    TestGuessExtensionDerivesFromUrl();

    // BuildAlbumArtExchangeSearchUrl
    TestBuildAlbumArtExchangeSearchUrlIncludesArtistAndAlbum();
    TestBuildAlbumArtExchangeSearchUrlHandlesMultipleFields();
    TestBuildAlbumArtExchangeSearchUrlHandlesEmptyFields();

    // ExtractCoverIdFromHtml
    TestExtractCoverIdFromHtmlFindsDataCoverId();
    TestExtractCoverIdFromHtmlReturnsEmptyWhenNotFound();
    TestExtractCoverIdFromHtmlAdvancesPosition();

    // ExtractSlugFromDetailLink
    TestExtractSlugFromDetailLinkExtractsSlug();
    TestExtractSlugFromDetailLinkReturnsEmptyForInvalidLinks();

    // BuildDiscogsSearchUrl with discogsType parameter
    TestBuildDiscogsSearchUrlUsesMasterTypeByDefault();
    TestBuildDiscogsSearchUrlAcceptsReleaseType();
    TestBuildDiscogsSearchUrlWithReleaseTypeIncludesAllFields();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    curl_global_cleanup();
    return EXIT_FAILURE;
  }

  curl_global_cleanup();
  return EXIT_SUCCESS;
}