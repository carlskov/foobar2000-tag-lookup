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

// ---------------------------------------------------------------------------
// JsonToString (tag_lookup_service)
// ---------------------------------------------------------------------------

void TestJsonToStringConvertsVariousTypes() {
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
// JsonFieldToString (tag_lookup_service)
// ---------------------------------------------------------------------------

void TestJsonFieldToStringReturnsFieldValue() {
  const auto json = nlohmann::json::parse(R"json({ "name": "Test", "count": 5 })json");
  Expect(taglookup::JsonFieldToString(json, "name") == "Test",
         "JsonFieldToString should return string field value");
  Expect(taglookup::JsonFieldToString(json, "count") == "5",
         "JsonFieldToString should convert numeric field to string");
  Expect(taglookup::JsonFieldToString(json, "missing") == "",
         "JsonFieldToString should return empty for missing field");
}

// ---------------------------------------------------------------------------
// Trim
// ---------------------------------------------------------------------------

void TestTrimRemovesLeadingAndTrailingWhitespace() {
  Expect(taglookup::Trim("  hello  ") == "hello", "Trim should remove surrounding spaces");
  Expect(taglookup::Trim("\t\r\nhello\n") == "hello", "Trim should remove surrounding mixed whitespace");
  Expect(taglookup::Trim("hello") == "hello", "Trim should leave already-trimmed string unchanged");
  Expect(taglookup::Trim("   ") == "", "Trim should return empty string for all-whitespace input");
  Expect(taglookup::Trim("") == "", "Trim should return empty string for empty input");
}

// ---------------------------------------------------------------------------
// StripDiscogsLabelSuffix
// ---------------------------------------------------------------------------

void TestStripDiscogsLabelSuffixRemovesCatNumber() {
  Expect(taglookup::StripDiscogsLabelSuffix("Parlophone - PCSD 115") == "Parlophone",
         "Should strip catalogue number after ' - '");
  Expect(taglookup::StripDiscogsLabelSuffix("Island Records") == "Island Records",
         "Label with no suffix should be returned unchanged");
  Expect(taglookup::StripDiscogsLabelSuffix("  XL Recordings  ") == "XL Recordings",
         "Should trim surrounding whitespace");
  Expect(taglookup::StripDiscogsLabelSuffix("  XL - 123  ") == "XL",
         "Should trim whitespace and strip suffix");
}

// ---------------------------------------------------------------------------
// ToLower
// ---------------------------------------------------------------------------

void TestToLowerConvertsUppercaseCharacters() {
  Expect(taglookup::ToLower("HELLO") == "hello", "ToLower should convert uppercase to lowercase");
  Expect(taglookup::ToLower("Hello World") == "hello world", "ToLower should handle mixed case");
  Expect(taglookup::ToLower("already lower") == "already lower",
         "ToLower should leave lowercase strings unchanged");
  Expect(taglookup::ToLower("") == "", "ToLower should handle empty string");
}

// ---------------------------------------------------------------------------
// NormalizeForMatch
// ---------------------------------------------------------------------------

void TestNormalizeForMatchKeepsOnlyAlphanumericLowercase() {
  Expect(taglookup::NormalizeForMatch("Hello, World!") == "helloworld",
         "NormalizeForMatch should keep only alphanumeric chars, lowercase");
  Expect(taglookup::NormalizeForMatch("OK Computer") == "okcomputer",
         "NormalizeForMatch should strip spaces");
  Expect(taglookup::NormalizeForMatch("1997") == "1997",
         "NormalizeForMatch should keep digits");
  Expect(taglookup::NormalizeForMatch("") == "", "NormalizeForMatch should handle empty string");
  Expect(taglookup::NormalizeForMatch("...!!!") == "",
         "NormalizeForMatch should return empty for punctuation-only input");
}

// ---------------------------------------------------------------------------
// ContainsCaseInsensitive
// ---------------------------------------------------------------------------

void TestContainsCaseInsensitiveIgnoresCase() {
  Expect(taglookup::ContainsCaseInsensitive("Paranoid Android", "android"),
         "ContainsCaseInsensitive should find lowercase needle in mixed-case haystack");
  Expect(taglookup::ContainsCaseInsensitive("Paranoid Android", "PARANOID"),
         "ContainsCaseInsensitive should find uppercase needle");
  Expect(!taglookup::ContainsCaseInsensitive("Radiohead", "coldplay"),
         "ContainsCaseInsensitive should return false when needle is absent");
  Expect(taglookup::ContainsCaseInsensitive("anything", ""),
         "ContainsCaseInsensitive with empty needle should return true");
}

// ---------------------------------------------------------------------------
// FuzzyContainsNormalized
// ---------------------------------------------------------------------------

void TestFuzzyContainsNormalizedIgnoresPunctuationAndCase() {
  Expect(taglookup::FuzzyContainsNormalized("OK Computer", "ok computer"),
         "FuzzyContainsNormalized should ignore case");
  Expect(taglookup::FuzzyContainsNormalized("OK Computer!", "OK Computer"),
         "FuzzyContainsNormalized should ignore trailing punctuation");
  Expect(taglookup::FuzzyContainsNormalized("OK. Computer", "OKComputer"),
         "FuzzyContainsNormalized should ignore internal punctuation");
  Expect(!taglookup::FuzzyContainsNormalized("Kid A", "OK Computer"),
         "FuzzyContainsNormalized should return false when needle absent");
  Expect(taglookup::FuzzyContainsNormalized("anything", ""),
         "FuzzyContainsNormalized with empty needle should return true");
}

// ---------------------------------------------------------------------------
// MatchesFilledFields – additional edge cases
// ---------------------------------------------------------------------------

void TestMatchesFilledFieldsEmptyQueryAlwaysMatches() {
  taglookup::LookupQuery query;  // All fields empty.
  taglookup::TagResult result;
  result.artist = "Radiohead";
  result.title = "Creep";
  result.album = "Pablo Honey";
  result.label = "Parlophone";
  result.date = "1993";

  Expect(taglookup::MatchesFilledFields(query, result),
         "Empty query should match any result");
}

void TestMatchesFilledFieldsArtistFieldUsedForFiltering() {
  taglookup::LookupQuery query;
  query.artist = "Radiohead";

  taglookup::TagResult match;
  match.artist = "Radiohead";

  taglookup::TagResult noMatch;
  noMatch.artist = "Coldplay";

  Expect(taglookup::MatchesFilledFields(query, match), "Artist should match");
  Expect(!taglookup::MatchesFilledFields(query, noMatch), "Artist mismatch should fail");
}

// ---------------------------------------------------------------------------
// BuildDiscogsGeneralQuery
// ---------------------------------------------------------------------------

void TestBuildDiscogsGeneralQueryCombinesNonEmptyFields() {
  taglookup::LookupQuery query;
  query.artist = "Radiohead";
  query.album = "OK Computer";
  query.title = "Paranoid Android";
  query.year = "1997";

  const std::string result = taglookup::BuildDiscogsGeneralQuery(query);
  Expect(result == "Radiohead OK Computer Paranoid Android 1997",
         "BuildDiscogsGeneralQuery should concatenate non-empty fields with spaces");
}

void TestBuildDiscogsGeneralQuerySkipsEmptyAndWhitespaceFields() {
  taglookup::LookupQuery query;
  query.artist = "Radiohead";
  query.album = "";
  query.title = "  ";  // Whitespace only – should be skipped.
  query.year = "1997";

  const std::string result = taglookup::BuildDiscogsGeneralQuery(query);
  Expect(result == "Radiohead 1997",
         "BuildDiscogsGeneralQuery should skip empty and whitespace-only fields");
}

// ---------------------------------------------------------------------------
// BuildMusicBrainzUrl
// ---------------------------------------------------------------------------

void TestBuildMusicBrainzUrlExactPhraseWrapsFieldsInQuotes() {
  CURL* curl = curl_easy_init();
  Expect(curl != nullptr, "curl_easy_init failed");

  taglookup::LookupQuery query;
  query.artist = "Radiohead";
  query.album = "OK Computer";
  query.title = "Paranoid Android";
  query.label = "Parlophone";
  query.year = "1997";

  const std::string url =
      taglookup::BuildMusicBrainzUrl(curl, query, 10, 0, taglookup::SearchMode::ExactPhrase);
  curl_easy_cleanup(curl);

  Expect(url.find("musicbrainz.org") != std::string::npos, "URL should target MusicBrainz");
  Expect(url.find("fmt=json") != std::string::npos, "URL should request JSON format");
  Expect(url.find("limit=10") != std::string::npos, "URL should include limit");
  Expect(url.find("offset=0") != std::string::npos, "URL should include offset");
  // ExactPhrase mode URL-encodes quoted clauses; verify key fields are present.
  Expect(url.find("artist") != std::string::npos, "URL should include artist clause");
  Expect(url.find("release") != std::string::npos, "URL should include release clause");
}

void TestBuildMusicBrainzUrlTokenizedModeOmitsQuotes() {
  CURL* curl = curl_easy_init();
  Expect(curl != nullptr, "curl_easy_init failed");

  taglookup::LookupQuery query;
  query.artist = "Radiohead";
  query.album = "OK Computer";

  const std::string exactUrl =
      taglookup::BuildMusicBrainzUrl(curl, query, 10, 0, taglookup::SearchMode::ExactPhrase);
  const std::string tokenizedUrl =
      taglookup::BuildMusicBrainzUrl(curl, query, 10, 0, taglookup::SearchMode::Tokenized);
  curl_easy_cleanup(curl);

  // ExactPhrase URL-encodes quotes (%22); Tokenized omits them.
  Expect(exactUrl.find("%22") != std::string::npos, "ExactPhrase URL should URL-encode quotes");
  Expect(tokenizedUrl.find("%22") == std::string::npos, "Tokenized URL should omit quotes");
}

// ---------------------------------------------------------------------------
// ExtractPrimaryArtist
// ---------------------------------------------------------------------------

void TestExtractPrimaryArtistReturnsFirstArtistCreditName() {
  const auto json = nlohmann::json::parse(R"json({
    "artist-credit": [
      { "name": "Radiohead" },
      { "name": "feat. Thom Yorke" }
    ]
  })json");

  Expect(taglookup::ExtractPrimaryArtist(json) == "Radiohead",
         "ExtractPrimaryArtist should return the name from the first artist-credit entry");
}

void TestExtractPrimaryArtistReturnsEmptyWhenNoCreditArray() {
  const auto json = nlohmann::json::parse(R"json({})json");
  Expect(taglookup::ExtractPrimaryArtist(json).empty(),
         "ExtractPrimaryArtist should return empty string when artist-credit is absent");
}

// ---------------------------------------------------------------------------
// ReadScore
// ---------------------------------------------------------------------------

void TestReadScoreHandlesIntegerScore() {
  const auto json = nlohmann::json::parse(R"json({ "score": 95 })json");
  Expect(taglookup::ReadScore(json) == 95, "ReadScore should parse integer score");
}

void TestReadScoreHandlesStringScore() {
  const auto json = nlohmann::json::parse(R"json({ "score": "87" })json");
  Expect(taglookup::ReadScore(json) == 87, "ReadScore should parse string score");
}

void TestReadScoreReturnsZeroWhenAbsent() {
  const auto json = nlohmann::json::parse(R"json({})json");
  Expect(taglookup::ReadScore(json) == 0, "ReadScore should return 0 when score field is absent");
}

// ---------------------------------------------------------------------------
// ExtractPrimaryLabel
// ---------------------------------------------------------------------------

void TestExtractPrimaryLabelReturnsFirstLabelName() {
  const auto json = nlohmann::json::parse(R"json({
    "label-info": [
      { "label": { "name": "Parlophone" } },
      { "label": { "name": "Capitol" } }
    ]
  })json");

  Expect(taglookup::ExtractPrimaryLabel(json) == "Parlophone",
         "ExtractPrimaryLabel should return the first label name");
}

void TestExtractPrimaryLabelReturnsEmptyWhenAbsent() {
  const auto json = nlohmann::json::parse(R"json({})json");
  Expect(taglookup::ExtractPrimaryLabel(json).empty(),
         "ExtractPrimaryLabel should return empty when label-info is absent");
}

// ---------------------------------------------------------------------------
// BuildMusicBrainzCoverUrl
// ---------------------------------------------------------------------------

void TestBuildMusicBrainzCoverUrlReturnsUrlWhenFrontIsAvailable() {
  const auto json = nlohmann::json::parse(R"json({
    "cover-art-archive": { "front": true }
  })json");
  const std::string url = taglookup::BuildMusicBrainzCoverUrl(json, "abc-123");
  Expect(url == "https://coverartarchive.org/release/abc-123/front-250",
         "BuildMusicBrainzCoverUrl should build the CAA URL when front art is available");
}

void TestBuildMusicBrainzCoverUrlReturnsEmptyWhenFrontIsFalse() {
  const auto json = nlohmann::json::parse(R"json({
    "cover-art-archive": { "front": false }
  })json");
  Expect(taglookup::BuildMusicBrainzCoverUrl(json, "abc-123").empty(),
         "BuildMusicBrainzCoverUrl should return empty when front cover flag is false");
}

void TestBuildMusicBrainzCoverUrlReturnsEmptyForEmptyReleaseId() {
  const auto json = nlohmann::json::parse(R"json({})json");
  Expect(taglookup::BuildMusicBrainzCoverUrl(json, "").empty(),
         "BuildMusicBrainzCoverUrl should return empty when release ID is empty");
}

// ---------------------------------------------------------------------------
// JoinArtistNames
// ---------------------------------------------------------------------------

void TestJoinArtistNamesUsesJoinField() {
  // The join field is read from the *current* artist entry when it is being
  // appended after an already-non-empty result, so the separator lives on the
  // second entry's "join" key.
  const auto json = nlohmann::json::parse(R"json([
    { "name": "DJ Format" },
    { "name": "Abdominal", "join": "&" }
  ])json");
  Expect(taglookup::JoinArtistNames(json) == "DJ Format & Abdominal",
         "JoinArtistNames should use the join field as separator");
}

void TestJoinArtistNamesFallsBackToCommaWhenNoJoin() {
  const auto json = nlohmann::json::parse(R"json([
    { "name": "Artist A" },
    { "name": "Artist B" }
  ])json");
  Expect(taglookup::JoinArtistNames(json) == "Artist A, Artist B",
         "JoinArtistNames should use ', ' when join field is absent");
}

void TestJoinArtistNamesReturnsEmptyForEmptyArray() {
  const auto json = nlohmann::json::parse(R"json([])json");
  Expect(taglookup::JoinArtistNames(json).empty(),
         "JoinArtistNames should return empty for empty array");
}

// ---------------------------------------------------------------------------
// ExtractDiscogsReleaseArtist
// ---------------------------------------------------------------------------

void TestExtractDiscogsReleaseArtistJoinsArtistNames() {
  const auto json = nlohmann::json::parse(R"json({
    "artists": [
      { "name": "Boards of Canada" }
    ]
  })json");
  Expect(taglookup::ExtractDiscogsReleaseArtist(json) == "Boards of Canada",
         "ExtractDiscogsReleaseArtist should return the joined artist names");
}

void TestExtractDiscogsReleaseArtistReturnsEmptyWhenAbsent() {
  const auto json = nlohmann::json::parse(R"json({})json");
  Expect(taglookup::ExtractDiscogsReleaseArtist(json).empty(),
         "ExtractDiscogsReleaseArtist should return empty when artists field is absent");
}

// ---------------------------------------------------------------------------
// ExtractDiscogsMasterCoverUrl – additional cases
// ---------------------------------------------------------------------------

void TestExtractDiscogsMasterCoverUrlFallsBackToUriWhenNo150() {
  const auto json = nlohmann::json::parse(R"json({
    "images": [
      { "uri": "https://img.example/full.jpg" }
    ]
  })json");
  Expect(taglookup::ExtractDiscogsMasterCoverUrl(json) == "https://img.example/full.jpg",
         "Should fall back to uri when uri150 is absent");
}

void TestExtractDiscogsMasterCoverUrlReturnsEmptyWhenNoImages() {
  const auto json = nlohmann::json::parse(R"json({})json");
  Expect(taglookup::ExtractDiscogsMasterCoverUrl(json).empty(),
         "Should return empty when images field is absent");
}

// ---------------------------------------------------------------------------
// ExtractDiscogsMediaType
// ---------------------------------------------------------------------------

void TestExtractDiscogsMediaTypeReturnsFirstFormatName() {
  const auto json = nlohmann::json::parse(R"json({
    "formats": [
      { "name": "Vinyl" },
      { "name": "CD" }
    ]
  })json");
  Expect(taglookup::ExtractDiscogsMediaType(json) == "Vinyl",
         "ExtractDiscogsMediaType should return the first format name");
}

void TestExtractDiscogsMediaTypeReturnsEmptyWhenAbsent() {
  const auto json = nlohmann::json::parse(R"json({})json");
  Expect(taglookup::ExtractDiscogsMediaType(json).empty(),
         "ExtractDiscogsMediaType should return empty when formats field is absent");
}

// ---------------------------------------------------------------------------
// ExtractDiscNumberFromDiscogsPosition
// ---------------------------------------------------------------------------

void TestExtractDiscNumberFromDiscogsPositionParsesNumericPrefix() {
  Expect(taglookup::ExtractDiscNumberFromDiscogsPosition("1-03") == "1",
         "Should extract disc 1 from '1-03'");
  Expect(taglookup::ExtractDiscNumberFromDiscogsPosition("2.07") == "2",
         "Should extract disc 2 from '2.07'");
  Expect(taglookup::ExtractDiscNumberFromDiscogsPosition("2:07") == "2",
         "Should extract disc 2 from '2:07'");
}

void TestExtractDiscNumberFromDiscogsPositionParsesCDPrefix() {
  Expect(taglookup::ExtractDiscNumberFromDiscogsPosition("CD1-4") == "1",
         "Should extract disc 1 from 'CD1-4'");
  Expect(taglookup::ExtractDiscNumberFromDiscogsPosition("CD2-1") == "2",
         "Should extract disc 2 from 'CD2-1'");
}

void TestExtractDiscNumberFromDiscogsPositionReturnsEmptyForPlainTrackNumber() {
  Expect(taglookup::ExtractDiscNumberFromDiscogsPosition("5").empty(),
         "Plain track number without separator should return empty disc number");
  Expect(taglookup::ExtractDiscNumberFromDiscogsPosition("").empty(),
         "Empty position should return empty disc number");
}

// ---------------------------------------------------------------------------
// JoinJsonStringArray
// ---------------------------------------------------------------------------

void TestJoinJsonStringArrayJoinsWithSemicolon() {
  const auto json = nlohmann::json::parse(R"json(["Rock", "Alternative", "Indie"])json");
  Expect(taglookup::JoinJsonStringArray(json) == "Rock; Alternative; Indie",
         "JoinJsonStringArray should join strings with '; '");
}

void TestJoinJsonStringArraySkipsNonStringElements() {
  const auto json = nlohmann::json::parse(R"json(["Rock", 42, "Indie"])json");
  Expect(taglookup::JoinJsonStringArray(json) == "Rock; Indie",
         "JoinJsonStringArray should skip non-string elements");
}

void TestJoinJsonStringArrayReturnsEmptyForEmptyArray() {
  const auto json = nlohmann::json::parse(R"json([])json");
  Expect(taglookup::JoinJsonStringArray(json).empty(),
         "JoinJsonStringArray should return empty for empty array");
}

// ---------------------------------------------------------------------------
// ParsePositiveInt
// ---------------------------------------------------------------------------

void TestParsePositiveIntReturnsValueForValidInput() {
  Expect(taglookup::ParsePositiveInt("42") == 42,
         "ParsePositiveInt should return 42 for '42'");
  Expect(taglookup::ParsePositiveInt("1") == 1,
         "ParsePositiveInt should return 1 for '1'");
}

void TestParsePositiveIntReturnsZeroForNonPositive() {
  Expect(taglookup::ParsePositiveInt("0") == 0,
         "ParsePositiveInt should return 0 for '0'");
  Expect(taglookup::ParsePositiveInt("-5") == 0,
         "ParsePositiveInt should return 0 for negative values");
  Expect(taglookup::ParsePositiveInt("") == 0,
         "ParsePositiveInt should return 0 for empty string");
  Expect(taglookup::ParsePositiveInt("abc") == 0,
         "ParsePositiveInt should return 0 for non-numeric input");
}

// ---------------------------------------------------------------------------
// FindFirstTrackTitle
// ---------------------------------------------------------------------------

void TestFindFirstTrackTitleReturnsFirstTrackInMedia() {
  const auto json = nlohmann::json::parse(R"json({
    "media": [
      {
        "tracks": [
          { "title": "Airbag" },
          { "title": "Paranoid Android" }
        ]
      }
    ]
  })json");
  Expect(taglookup::FindFirstTrackTitle(json) == "Airbag",
         "FindFirstTrackTitle should return the first non-empty track title from media");
}

void TestFindFirstTrackTitleFallsBackToTracklist() {
  const auto json = nlohmann::json::parse(R"json({
    "tracklist": [
      { "title": "Karma Police", "type_": "track" },
      { "title": "No Surprises" }
    ]
  })json");
  Expect(taglookup::FindFirstTrackTitle(json) == "Karma Police",
         "FindFirstTrackTitle should return first track from tracklist");
}

void TestFindFirstTrackTitleReturnsEmptyWhenNoTracks() {
  const auto json = nlohmann::json::parse(R"json({})json");
  Expect(taglookup::FindFirstTrackTitle(json).empty(),
         "FindFirstTrackTitle should return empty when no tracks are present");
}

// ---------------------------------------------------------------------------
// FindMatchingTrackTitle
// ---------------------------------------------------------------------------

void TestFindMatchingTrackTitleMatchesFuzzilyFromMedia() {
  const auto json = nlohmann::json::parse(R"json({
    "media": [
      {
        "tracks": [
          { "title": "Airbag" },
          { "title": "Paranoid Android" }
        ]
      }
    ]
  })json");
  Expect(taglookup::FindMatchingTrackTitle(json, "Paranoid Android") == "Paranoid Android",
         "FindMatchingTrackTitle should return the matching track title");
  Expect(taglookup::FindMatchingTrackTitle(json, "paranoid android") == "Paranoid Android",
         "FindMatchingTrackTitle should match case-insensitively");
}

void TestFindMatchingTrackTitleReturnsEmptyWhenNoMatch() {
  const auto json = nlohmann::json::parse(R"json({
    "media": [
      {
        "tracks": [
          { "title": "Airbag" }
        ]
      }
    ]
  })json");
  Expect(taglookup::FindMatchingTrackTitle(json, "Karma Police").empty(),
         "FindMatchingTrackTitle should return empty when no track matches");
}

// ---------------------------------------------------------------------------
// ResolveTrackTitleFromReleaseJson
// ---------------------------------------------------------------------------

void TestResolveTrackTitleFromReleaseJsonReturnsMatchedTitle() {
  const auto json = nlohmann::json::parse(R"json({
    "media": [
      {
        "tracks": [
          { "title": "Exit Music (For a Film)" },
          { "title": "Let Down" }
        ]
      }
    ]
  })json");
  const std::string resolved = taglookup::ResolveTrackTitleFromReleaseJson(json, "Exit Music");
  Expect(resolved == "Exit Music (For a Film)",
         "ResolveTrackTitleFromReleaseJson should return the matched release title");
}

void TestResolveTrackTitleFromReleaseJsonFallsBackToWantedWhenNoMatch() {
  const auto json = nlohmann::json::parse(R"json({
    "media": [{ "tracks": [{ "title": "Airbag" }] }]
  })json");
  const std::string resolved = taglookup::ResolveTrackTitleFromReleaseJson(json, "Karma Police");
  Expect(resolved == "Karma Police",
         "ResolveTrackTitleFromReleaseJson should fall back to the wantedTitle when no match");
}

void TestResolveTrackTitleFromReleaseJsonReturnsFirstTrackWhenWantedEmpty() {
  const auto json = nlohmann::json::parse(R"json({
    "media": [{ "tracks": [{ "title": "Airbag" }, { "title": "Paranoid Android" }] }]
  })json");
  const std::string resolved = taglookup::ResolveTrackTitleFromReleaseJson(json, "");
  Expect(resolved == "Airbag",
         "ResolveTrackTitleFromReleaseJson should return first track title when wantedTitle is empty");
}

// ---------------------------------------------------------------------------
// FindMatchingDiscogsTrack
// ---------------------------------------------------------------------------

void TestFindMatchingDiscogsTrackFindsMatchByTitle() {
  const auto json = nlohmann::json::parse(R"json({
    "tracklist": [
      { "title": "Untitled", "type_": "track" },
      { "title": "Roygbiv", "type_": "track" }
    ]
  })json");
  const nlohmann::json* track = taglookup::FindMatchingDiscogsTrack(json, "roygbiv");
  Expect(track != nullptr, "FindMatchingDiscogsTrack should find a matching track");
  Expect(taglookup::JsonFieldToString(*track, "title") == "Roygbiv",
         "FindMatchingDiscogsTrack should return the correct track");
}

void TestFindMatchingDiscogsTrackReturnsFirstTrackWhenNoTitleMatch() {
  const auto json = nlohmann::json::parse(R"json({
    "tracklist": [
      { "title": "Untitled", "type_": "track" },
      { "title": "Roygbiv", "type_": "track" }
    ]
  })json");
  const nlohmann::json* track = taglookup::FindMatchingDiscogsTrack(json, "nonexistent");
  Expect(track != nullptr, "FindMatchingDiscogsTrack should return first track when no match");
  Expect(taglookup::JsonFieldToString(*track, "title") == "Untitled",
         "FindMatchingDiscogsTrack should return first track as fallback");
}

void TestFindMatchingDiscogsTrackSkipsHeaderTypeEntries() {
  const auto json = nlohmann::json::parse(R"json({
    "tracklist": [
      { "title": "Side A", "type_": "heading" },
      { "title": "Untitled", "type_": "track" }
    ]
  })json");
  const nlohmann::json* track = taglookup::FindMatchingDiscogsTrack(json, "");
  Expect(track != nullptr, "FindMatchingDiscogsTrack should skip heading entries");
  Expect(taglookup::JsonFieldToString(*track, "title") == "Untitled",
         "FindMatchingDiscogsTrack should skip heading and return first real track");
}

// ---------------------------------------------------------------------------
// ResolveDiscogsTrackArtistFromReleaseJson
// ---------------------------------------------------------------------------

void TestResolveDiscogsTrackArtistUsesTrackLevelArtistWhenAvailable() {
  const auto json = nlohmann::json::parse(R"json({
    "artists": [{ "name": "Various Artists" }],
    "tracklist": [
      {
        "title": "Blue Monday",
        "type_": "track",
        "artists": [{ "name": "New Order" }]
      }
    ]
  })json");
  const std::string artist =
      taglookup::ResolveDiscogsTrackArtistFromReleaseJson(json, "Blue Monday", "fallback");
  Expect(artist == "New Order",
         "Should use track-level artist when available");
}

void TestResolveDiscogsTrackArtistFallsBackToReleaseArtist() {
  const auto json = nlohmann::json::parse(R"json({
    "artists": [{ "name": "Radiohead" }],
    "tracklist": [
      { "title": "Karma Police", "type_": "track" }
    ]
  })json");
  const std::string artist =
      taglookup::ResolveDiscogsTrackArtistFromReleaseJson(json, "Karma Police", "fallback");
  Expect(artist == "Radiohead",
         "Should fall back to release-level artist when track has no artist");
}

void TestResolveDiscogsTrackArtistFallsBackToProvidedFallback() {
  const auto json = nlohmann::json::parse(R"json({
    "tracklist": [
      { "title": "Track A", "type_": "track" }
    ]
  })json");
  const std::string artist =
      taglookup::ResolveDiscogsTrackArtistFromReleaseJson(json, "Track A", "My Fallback");
  Expect(artist == "My Fallback",
         "Should use the provided fallback when no release or track artist is present");
}

// ---------------------------------------------------------------------------
// BuildDiscogsUrl with discogsType parameter
// ---------------------------------------------------------------------------

void TestBuildDiscogsUrlUsesMasterTypeByDefault() {
  CURL* curl = curl_easy_init();
  Expect(curl != nullptr, "curl_easy_init failed");

  taglookup::LookupQuery query;
  query.artist = "Radiohead";
  query.album = "OK Computer";

  const std::string url = taglookup::BuildDiscogsUrl(
      curl, query, 10, 1, taglookup::SearchMode::ExactPhrase, false, false);
  curl_easy_cleanup(curl);

  Expect(url.find("type=master") != std::string::npos,
         "Default Discogs lookup should use type=master");
}

void TestBuildDiscogsUrlAcceptsReleaseType() {
  CURL* curl = curl_easy_init();
  Expect(curl != nullptr, "curl_easy_init failed");

  taglookup::LookupQuery query;
  query.artist = "Radiohead";
  query.album = "OK Computer";

  const std::string url = taglookup::BuildDiscogsUrl(
      curl, query, 10, 1, taglookup::SearchMode::ExactPhrase, false, false, "release");
  curl_easy_cleanup(curl);

  Expect(url.find("type=release") != std::string::npos,
         "Discogs lookup with discogsType='release' should use type=release");
}

void TestBuildDiscogsUrlWithReleaseTypeIncludesAllFields() {
  CURL* curl = curl_easy_init();
  Expect(curl != nullptr, "curl_easy_init failed");

  taglookup::LookupQuery query;
  query.artist = "Radiohead";
  query.album = "OK Computer";
  query.label = "Parlophone";
  query.year = "1997";

  const std::string url = taglookup::BuildDiscogsUrl(
      curl, query, 10, 1, taglookup::SearchMode::ExactPhrase, false, false, "release");
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
    TestDiscogsMasterUrlUsesMasterSearchAndOmitsTrackByDefault();
    TestDiscogsGeneralQueryIncludesAllFilledFields();
    TestMatchesFilledFieldsRequiresAllProvidedFields();
    TestExtractDiscogsMasterCoverUrlPrefersUri150ForPreviews();

    // BuildDiscogsUrl with discogsType parameter
    TestBuildDiscogsUrlUsesMasterTypeByDefault();
    TestBuildDiscogsUrlAcceptsReleaseType();
    TestBuildDiscogsUrlWithReleaseTypeIncludesAllFields();

    // JsonToString
    TestJsonToStringConvertsVariousTypes();

    // JsonFieldToString
    TestJsonFieldToStringReturnsFieldValue();

    // Trim
    TestTrimRemovesLeadingAndTrailingWhitespace();

    // StripDiscogsLabelSuffix
    TestStripDiscogsLabelSuffixRemovesCatNumber();

    // ToLower
    TestToLowerConvertsUppercaseCharacters();

    // NormalizeForMatch
    TestNormalizeForMatchKeepsOnlyAlphanumericLowercase();

    // ContainsCaseInsensitive
    TestContainsCaseInsensitiveIgnoresCase();

    // FuzzyContainsNormalized
    TestFuzzyContainsNormalizedIgnoresPunctuationAndCase();

    // MatchesFilledFields – additional cases
    TestMatchesFilledFieldsEmptyQueryAlwaysMatches();
    TestMatchesFilledFieldsArtistFieldUsedForFiltering();

    // BuildDiscogsGeneralQuery
    TestBuildDiscogsGeneralQueryCombinesNonEmptyFields();
    TestBuildDiscogsGeneralQuerySkipsEmptyAndWhitespaceFields();

    // BuildMusicBrainzUrl
    TestBuildMusicBrainzUrlExactPhraseWrapsFieldsInQuotes();
    TestBuildMusicBrainzUrlTokenizedModeOmitsQuotes();

    // ExtractPrimaryArtist
    TestExtractPrimaryArtistReturnsFirstArtistCreditName();
    TestExtractPrimaryArtistReturnsEmptyWhenNoCreditArray();

    // ReadScore
    TestReadScoreHandlesIntegerScore();
    TestReadScoreHandlesStringScore();
    TestReadScoreReturnsZeroWhenAbsent();

    // ExtractPrimaryLabel
    TestExtractPrimaryLabelReturnsFirstLabelName();
    TestExtractPrimaryLabelReturnsEmptyWhenAbsent();

    // BuildMusicBrainzCoverUrl
    TestBuildMusicBrainzCoverUrlReturnsUrlWhenFrontIsAvailable();
    TestBuildMusicBrainzCoverUrlReturnsEmptyWhenFrontIsFalse();
    TestBuildMusicBrainzCoverUrlReturnsEmptyForEmptyReleaseId();

    // JoinArtistNames
    TestJoinArtistNamesUsesJoinField();
    TestJoinArtistNamesFallsBackToCommaWhenNoJoin();
    TestJoinArtistNamesReturnsEmptyForEmptyArray();

    // ExtractDiscogsReleaseArtist
    TestExtractDiscogsReleaseArtistJoinsArtistNames();
    TestExtractDiscogsReleaseArtistReturnsEmptyWhenAbsent();

    // ExtractDiscogsMasterCoverUrl – additional cases
    TestExtractDiscogsMasterCoverUrlFallsBackToUriWhenNo150();
    TestExtractDiscogsMasterCoverUrlReturnsEmptyWhenNoImages();

    // ExtractDiscogsMediaType
    TestExtractDiscogsMediaTypeReturnsFirstFormatName();
    TestExtractDiscogsMediaTypeReturnsEmptyWhenAbsent();

    // ExtractDiscNumberFromDiscogsPosition
    TestExtractDiscNumberFromDiscogsPositionParsesNumericPrefix();
    TestExtractDiscNumberFromDiscogsPositionParsesCDPrefix();
    TestExtractDiscNumberFromDiscogsPositionReturnsEmptyForPlainTrackNumber();

    // JoinJsonStringArray
    TestJoinJsonStringArrayJoinsWithSemicolon();
    TestJoinJsonStringArraySkipsNonStringElements();
    TestJoinJsonStringArrayReturnsEmptyForEmptyArray();

    // ParsePositiveInt
    TestParsePositiveIntReturnsValueForValidInput();
    TestParsePositiveIntReturnsZeroForNonPositive();

    // FindFirstTrackTitle
    TestFindFirstTrackTitleReturnsFirstTrackInMedia();
    TestFindFirstTrackTitleFallsBackToTracklist();
    TestFindFirstTrackTitleReturnsEmptyWhenNoTracks();

    // FindMatchingTrackTitle
    TestFindMatchingTrackTitleMatchesFuzzilyFromMedia();
    TestFindMatchingTrackTitleReturnsEmptyWhenNoMatch();

    // ResolveTrackTitleFromReleaseJson
    TestResolveTrackTitleFromReleaseJsonReturnsMatchedTitle();
    TestResolveTrackTitleFromReleaseJsonFallsBackToWantedWhenNoMatch();
    TestResolveTrackTitleFromReleaseJsonReturnsFirstTrackWhenWantedEmpty();

    // FindMatchingDiscogsTrack
    TestFindMatchingDiscogsTrackFindsMatchByTitle();
    TestFindMatchingDiscogsTrackReturnsFirstTrackWhenNoTitleMatch();
    TestFindMatchingDiscogsTrackSkipsHeaderTypeEntries();

    // ResolveDiscogsTrackArtistFromReleaseJson
    TestResolveDiscogsTrackArtistUsesTrackLevelArtistWhenAvailable();
    TestResolveDiscogsTrackArtistFallsBackToReleaseArtist();
    TestResolveDiscogsTrackArtistFallsBackToProvidedFallback();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    curl_global_cleanup();
    return EXIT_FAILURE;
  }

  curl_global_cleanup();
  return EXIT_SUCCESS;
}