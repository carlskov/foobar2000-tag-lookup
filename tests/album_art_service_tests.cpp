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

}  // namespace

int main() {
  curl_global_init(CURL_GLOBAL_DEFAULT);

  try {
    TestDiscogsAlbumArtSearchUsesMasters();
    TestGuessExtensionNormalizesJpegToJpg();
    TestExtractDiscogsMasterCoverUrlPrefersFullSizeImage();
    TestLooksLikeImageBytesRecognizesCommonFormats();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    curl_global_cleanup();
    return EXIT_FAILURE;
  }

  curl_global_cleanup();
  return EXIT_SUCCESS;
}