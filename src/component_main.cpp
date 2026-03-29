#include <foobar2000/SDK/foobar2000.h>

#include "album_art_service.h"
#include "album_art_selector.h"
#include "match_selector.h"
#include "search_input.h"
#include "tag_lookup_service.h"

#include <array>
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr GUID kMenuCommandGuid =
    {0x7fc620ce, 0x9fd4, 0x4f7f, {0xab, 0x8e, 0x43, 0xac, 0x03, 0x68, 0x52, 0xb2}};
constexpr GUID kAlbumArtMenuCommandGuid =
  {0x68ca89d8, 0x4fa8, 0x455f, {0x89, 0x4e, 0xcd, 0xbc, 0xdd, 0x5c, 0x22, 0x9f}};
constexpr GUID kSearchProviderSettingGuid =
    {0xc1138cbc, 0x6553, 0x4b14, {0xa6, 0xd7, 0x7e, 0x22, 0xc9, 0xde, 0x39, 0x20}};
constexpr GUID kSearchModeSettingGuid =
    {0x02a030e9, 0x2ca3, 0x4a9d, {0x83, 0x3e, 0x49, 0x0f, 0x66, 0xd5, 0xbe, 0xb2}};
constexpr GUID kOverwriteTitleSettingGuid =
    {0x09d1d2fc, 0x9e24, 0x4528, {0xbc, 0xb8, 0x53, 0x8e, 0x9d, 0x63, 0x54, 0x71}};
constexpr GUID kArtFetchProviderSettingGuid =
  {0x72cf060c, 0xd2f2, 0x43af, {0x84, 0x90, 0xd8, 0x3c, 0xe0, 0xd4, 0x1f, 0xe8}};
constexpr GUID kArtFetchSearchModeSettingGuid =
  {0x307f4ac8, 0x7f50, 0x4fb6, {0xa0, 0x74, 0x89, 0x08, 0x3f, 0x4c, 0xa5, 0xdc}};

cfg_int g_searchProviderSetting(kSearchProviderSettingGuid,
                                static_cast<int>(taglookup::LookupProvider::MusicBrainz));
cfg_int g_searchModeSetting(kSearchModeSettingGuid,
                            static_cast<int>(taglookup::SearchMode::ExactPhrase));
cfg_bool g_overwriteTitleSetting(kOverwriteTitleSettingGuid, false);
cfg_int g_artFetchProviderSetting(kArtFetchProviderSettingGuid,
                                  static_cast<int>(taglookup::AlbumArtProvider::MusicBrainz));
cfg_int g_artFetchSearchModeSetting(kArtFetchSearchModeSettingGuid,
                                    static_cast<int>(taglookup::SearchMode::ExactPhrase));

std::string Trim(std::string s) {
  const size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

template <typename TQuery>
bool ParseArtistTitle(const std::string& value, TQuery& out) {
  const size_t sep = value.find(" - ");
  if (sep == std::string::npos) {
    return false;
  }

  const std::string artist = Trim(value.substr(0, sep));
  const std::string title = Trim(value.substr(sep + 3));
  if (artist.empty() || title.empty()) {
    return false;
  }

  out.artist = artist;
  out.title = title;
  return true;
}

std::string TryReadClipboardArtistTitle() {
#if defined(__APPLE__)
  std::array<char, 256> buffer{};
  std::string text;

  FILE* pipe = popen("pbpaste", "r");
  if (pipe == nullptr) {
    return "";
  }

  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    text.append(buffer.data());
  }

  (void)pclose(pipe);
  return Trim(text);
#else
  return "";
#endif
}

template <typename TQuery>
bool TryBuildQueryFromFilename(const metadb_handle_ptr& track, TQuery& out) {
  const char* path = track->get_path();
  if (path == nullptr) {
    return false;
  }

  pfc::string8 filename(path);
  const char* base = pfc::string_filename_ext(filename);
  std::string stem = (base != nullptr) ? std::string(base) : std::string();

  const size_t dot = stem.find_last_of('.');
  if (dot != std::string::npos) {
    stem.resize(dot);
  }

  return ParseArtistTitle(stem, out);
}

class ReleaseTagPropagationFilter : public file_info_filter {
 public:
  ReleaseTagPropagationFilter(const pfc::list_base_const_t<metadb_handle_ptr>& data,
                              std::string albumArtist,
                              std::string totalTracks,
                              std::string totalDiscs,
                              std::string genre,
                              std::string label,
                              const taglookup::TagResult& result,
                              std::vector<taglookup::TrackInfo> tracks,
                              bool overwriteTitle)
      : album_artist_(std::move(albumArtist)), total_tracks_(std::move(totalTracks)),
        total_discs_(std::move(totalDiscs)), genre_(std::move(genre)), label_(std::move(label)),
        result_(result), tracks_(std::move(tracks)), overwrite_title_(overwriteTitle) {
    // Pre-build a map from each handle's raw pointer to its position in the selection.
    // apply_filter receives the same pointer, allowing per-file track resolution.
    for (size_t i = 0; i < data.get_count(); ++i) {
      handle_to_index_[data.get_item(i).get_ptr()] = i;
    }
  }

  bool apply_filter(metadb_handle_ptr location, t_filestats stats, file_info& info) override {
    (void)stats;

    bool changed = false;

    auto setIfNonEmpty = [&](const char* field, const std::string& value) {
      if (value.empty()) {
        return;
      }
      info.meta_set(field, value.c_str());
      changed = true;
    };

    // Resolve per-track title and artist based on this file's position in the selection.
    std::string trackTitle;
    std::string trackArtist = album_artist_;
    std::string trackNumber;
    std::string discNumber;
    std::string mediaType;

    const auto it = handle_to_index_.find(location.get_ptr());
    if (it != handle_to_index_.end() && it->second < tracks_.size()) {
      const auto& track = tracks_[it->second];
      trackTitle = track.title;
      trackNumber = track.trackNumber;
      discNumber = track.discNumber;
      mediaType = track.mediaType;
      if (!track.artist.empty()) {
        trackArtist = track.artist;
      }
    }

    setIfNonEmpty("ARTIST", trackArtist);
    setIfNonEmpty("ALBUM ARTIST", album_artist_);
    setIfNonEmpty("ALBUM", result_.album);
    setIfNonEmpty("DATE", result_.date);
    setIfNonEmpty("LABEL", label_);
    setIfNonEmpty("TOTALTRACKS", total_tracks_);
    setIfNonEmpty("TOTALDISCS", total_discs_);
    setIfNonEmpty("GENRE", genre_);
    setIfNonEmpty("TRACKNUMBER", trackNumber);
    setIfNonEmpty("DISCNUMBER", discNumber);
    setIfNonEmpty("MEDIA", mediaType);

    if (overwrite_title_) {
      setIfNonEmpty("TITLE", trackTitle);
    }

    if (!result_.release_id.empty()) {
      setIfNonEmpty("MUSICBRAINZ_RELEASEID", result_.release_id);
      setIfNonEmpty("DISCOGS_RELEASE_ID", result_.release_id);
    }

    return changed;
  }

 private:
  std::string album_artist_;
  std::string total_tracks_;
  std::string total_discs_;
  std::string genre_;
  std::string label_;
  taglookup::TagResult result_;
  std::vector<taglookup::TrackInfo> tracks_;
  bool overwrite_title_ = false;
  std::unordered_map<metadb_handle*, size_t> handle_to_index_;
};

void PropagateTagsToSelection(const pfc::list_base_const_t<metadb_handle_ptr>& data,
                              const std::string& albumArtist,
                              const std::string& totalTracks,
                              const std::string& totalDiscs,
                              const std::string& genre,
                              const std::string& label,
                              const taglookup::TagResult& result,
                              const std::vector<taglookup::TrackInfo>& tracks,
                              bool overwriteTitle) {
  const auto wndParent = core_api::get_main_window();
  auto filter = fb2k::service_new<ReleaseTagPropagationFilter>(data, albumArtist, totalTracks,
                                                               totalDiscs, genre, label, result,
                                                               tracks, overwriteTitle);

  auto notify = fb2k::makeCompletionNotify([](unsigned code) {
    FB2K_console_formatter() << "Tag propagation finished, code: " << code;
  });

  const uint32_t flags = metadb_io_v2::op_flag_partial_info_aware;
  metadb_io_v2::get()->update_info_async(data, filter, wndParent, flags, notify);
}

taglookup::LookupProvider LoadSearchProviderSetting() {
  const int value = static_cast<int>(g_searchProviderSetting);
  if (value == static_cast<int>(taglookup::LookupProvider::Discogs)) {
    return taglookup::LookupProvider::Discogs;
  }
  return taglookup::LookupProvider::MusicBrainz;
}

taglookup::SearchMode LoadSearchModeSetting() {
  const int value = static_cast<int>(g_searchModeSetting);
  if (value == static_cast<int>(taglookup::SearchMode::Tokenized)) {
    return taglookup::SearchMode::Tokenized;
  }
  return taglookup::SearchMode::ExactPhrase;
}

void StoreLookupSettings(const taglookup::LookupQuery& query) {
  g_searchProviderSetting = static_cast<int>(query.provider);
  g_searchModeSetting = static_cast<int>(query.search_mode);
  g_overwriteTitleSetting = query.overwrite_title_on_propagation;
}

taglookup::AlbumArtProvider LoadAlbumArtProviderSetting() {
  const int providerValue = static_cast<int>(g_artFetchProviderSetting);
  if (providerValue == static_cast<int>(taglookup::AlbumArtProvider::Discogs)) {
    return taglookup::AlbumArtProvider::Discogs;
  }
  return taglookup::AlbumArtProvider::MusicBrainz;
}

taglookup::SearchMode LoadAlbumArtSearchModeSetting() {
  const int modeValue = static_cast<int>(g_artFetchSearchModeSetting);
  if (modeValue == static_cast<int>(taglookup::SearchMode::Tokenized)) {
    return taglookup::SearchMode::Tokenized;
  }
  return taglookup::SearchMode::ExactPhrase;
}

void StoreAlbumArtSettings(const taglookup::AlbumArtQuery& query) {
  g_artFetchProviderSetting = static_cast<int>(query.provider);
  g_artFetchSearchModeSetting = static_cast<int>(query.search_mode);
}

std::filesystem::path ToFilesystemPath(const metadb_handle_ptr& track) {
  const char* rawPath = track->get_path();
  if (rawPath == nullptr) {
    return {};
  }

  pfc::string8 nativePath;
  if (foobar2000_io::extract_native_path_archive_aware(rawPath, nativePath)) {
    return std::filesystem::path(nativePath.c_str());
  }

  if (filesystem::g_get_native_path(rawPath, nativePath)) {
    return std::filesystem::path(nativePath.c_str());
  }

  if (pfc::strcmp_partial(rawPath, "file://") == 0) {
    return {};
  }

  return std::filesystem::path(rawPath);
}

bool SaveBytesToFile(const std::filesystem::path& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  return out.good();
}

void FetchAndSaveAlbumArt(const pfc::list_base_const_t<metadb_handle_ptr>& data,
                          const taglookup::AlbumArtQuery& query) {
  taglookup::AlbumArtService service;
  const auto candidates = service.FindAlbumArt(query, 10);
  if (candidates.empty()) {
    popup_message::g_show("No album art candidates found.", "Fetch Album Art");
    return;
  }

  const auto selectedIndex = taglookup::SelectAlbumArtCandidateIndex(query, candidates);
  if (!selectedIndex.has_value()) {
    return;
  }

  const auto& candidate = candidates[*selectedIndex];

  std::string bytes;
  std::string contentType;
  if (!service.DownloadBytes(candidate.url, bytes, contentType)) {
    popup_message::g_show("Failed to download album art bytes.", "Fetch Album Art");
    return;
  }

  const std::string ext = taglookup::AlbumArtService::GuessExtension(
      candidate.url, contentType, candidate.extension_hint);
  size_t saved = 0;
  size_t resolved = 0;
  size_t writeFailures = 0;
  std::unordered_set<std::string> writtenFolders;
  std::string sampleOutputPath;

  for (size_t i = 0; i < data.get_count(); ++i) {
    const auto path = ToFilesystemPath(data.get_item(i));
    if (path.empty()) {
      continue;
    }
    const auto dir = path.parent_path();
    if (dir.empty()) {
      continue;
    }
    const std::string dirKey = dir.string();
    if (!writtenFolders.insert(dirKey).second) {
      continue;
    }
    ++resolved;
    const auto output = dir / std::filesystem::path(std::string("cover") + ext);
    if (sampleOutputPath.empty()) {
      sampleOutputPath = output.string();
    }
    if (SaveBytesToFile(output, bytes)) {
      ++saved;
    } else {
      ++writeFailures;
    }
  }

  pfc::string_formatter status;
  if (saved == 0) {
    status << "Saved album art to 0 folder(s).\n";
    status << "Resolved writable folders: " << resolved << "\n";
    status << "Write failures: " << writeFailures << "\n";
    if (!sampleOutputPath.empty()) {
      status << "Sample output path: " << sampleOutputPath.c_str();
    } else {
      status << "No local filesystem paths could be resolved from the selected tracks.";
    }
  } else {
    status << "Saved album art to " << saved << " folder(s).";
    if (!sampleOutputPath.empty()) {
      status << "\nSample output path: " << sampleOutputPath.c_str();
    }
  }

  FB2K_console_formatter() << "Album art save result: saved=" << saved
                           << ", resolved=" << resolved
                           << ", writeFailures=" << writeFailures;
  if (!sampleOutputPath.empty()) {
    FB2K_console_formatter() << "Album art sample output path: " << sampleOutputPath.c_str();
  }
  popup_message::g_show(status.c_str(), "Fetch Album Art");
}

class ContextTagLookup : public contextmenu_item_simple {
 public:
  unsigned get_num_items() override { return 1; }

  void get_item_name(unsigned index, pfc::string_base& out) override {
    if (index == 0) {
      out = "Lookup Tags Online";
    }
  }

  void context_command(unsigned index, pfc::list_base_const_t<metadb_handle_ptr> const& data,
                       const GUID& caller) override {
    if (index != 0 || data.get_count() == 0) {
      return;
    }

    (void)caller;

    // Query behavior:
    // 1) Build defaults from clipboard / filename.
    // 2) Ask user for artist/release/track/year input.
    // 3) Lookup tags using MusicBrainz web API, matching all filled fields.
    // 4) Let user choose release from candidates.
    const metadb_handle_ptr track = data.get_item(0);

    taglookup::LookupQuery seed;
    const std::string clipboardText = TryReadClipboardArtistTitle();

    bool haveSeed = false;
    if (!clipboardText.empty()) {
      haveSeed = ParseArtistTitle(clipboardText, seed);
    }
    if (!haveSeed) {
      haveSeed = TryBuildQueryFromFilename(track, seed);
    }

    seed.provider = LoadSearchProviderSetting();
    seed.search_mode = LoadSearchModeSetting();
    seed.overwrite_title_on_propagation = static_cast<bool>(g_overwriteTitleSetting);

    taglookup::TagLookupService service;
    const auto queryOpt = taglookup::PromptForLookupQuery(seed, "");
    if (!queryOpt.has_value()) {
      return;
    }

    const taglookup::LookupQuery query = *queryOpt;
    StoreLookupSettings(query);
    seed = query;

    const std::vector<taglookup::TagResult> matches = service.LookupAll(query, 50);
    if (matches.empty()) {
      popup_message::g_show("No results found. Adjust fields and run Lookup Tags Online again.",
                            "Tag Lookup");
      return;
    }

    const auto selectedIndex = taglookup::SelectTagResultIndex(query, matches);
    if (!selectedIndex.has_value()) {
      return;
    }

      const auto& result = matches[*selectedIndex];

      // Fetch the full, ordered tracklist for the selected release in one API call.
      // This gives us per-track title and artist so each selected file gets its own tags.
      const taglookup::TracklistResult tracklistResult = service.FetchTracklist(query, result);
      const std::string albumArtist =
        tracklistResult.albumArtist.empty() ? result.artist : tracklistResult.albumArtist;
      const std::string effectiveLabel =
        tracklistResult.label.empty() ? result.label : tracklistResult.label;

      PropagateTagsToSelection(data, albumArtist, tracklistResult.totalTracks,
                   tracklistResult.totalDiscs, tracklistResult.genre, effectiveLabel, result,
                   tracklistResult.tracks, query.overwrite_title_on_propagation);

      pfc::string_formatter msg;
      msg << "Applying selected release tags to " << static_cast<unsigned>(data.get_count())
        << " file(s) in background.\n\n";
      msg << "Album Artist: " << albumArtist.c_str() << "\n";
      msg << "Album: " << result.album.c_str() << "\n";
      msg << "Genre: " << tracklistResult.genre.c_str() << "\n";
      msg << "Total tracks: " << tracklistResult.totalTracks.c_str() << "\n";
      msg << "Total discs: " << tracklistResult.totalDiscs.c_str() << "\n";
      if (!tracklistResult.tracks.empty()) {
        msg << "Tracks resolved: " << static_cast<unsigned>(tracklistResult.tracks.size())
          << " (each file gets its own per-track title/artist)\n";
      }
      msg << "Label: " << result.label.c_str() << "\n";
      msg << "Date: " << result.date.c_str() << "\n";
      msg << "Release ID: " << result.release_id.c_str() << "\n";
      msg << "Overwrite TITLE: "
        << (query.overwrite_title_on_propagation ? "Yes (per-track)" : "No (kept per-file titles)");

      popup_message::g_show(msg, "Tag Lookup: Propagating Tags");
      return;
  }

  GUID get_item_guid(unsigned index) override {
    if (index == 0) {
      return kMenuCommandGuid;
    }
    return pfc::guid_null;
  }

  bool get_item_description(unsigned index, pfc::string_base& out) override {
    if (index == 0) {
      out = "Fetch tags from online metadata database (manual term from clipboard or filename fallback).";
      return true;
    }
    return false;
  }

  GUID get_parent() override { return contextmenu_groups::tagging; }
};

class ContextAlbumArtFetch : public contextmenu_item_simple {
 public:
  unsigned get_num_items() override { return 1; }

  void get_item_name(unsigned index, pfc::string_base& out) override {
    if (index == 0) {
      out = "Fetch Album Art";
    }
  }

  void context_command(unsigned index, pfc::list_base_const_t<metadb_handle_ptr> const& data,
                       const GUID& caller) override {
    if (index != 0 || data.get_count() == 0) {
      return;
    }

    (void)caller;

    taglookup::AlbumArtQuery seed;
    const std::string clipboardText = TryReadClipboardArtistTitle();
    bool haveSeed = false;
    if (!clipboardText.empty()) {
      haveSeed = ParseArtistTitle(clipboardText, seed);
    }
    if (!haveSeed) {
      haveSeed = TryBuildQueryFromFilename(data.get_item(0), seed);
    }

    seed.provider = LoadAlbumArtProviderSetting();
    seed.search_mode = LoadAlbumArtSearchModeSetting();

    auto query = taglookup::PromptForAlbumArtQuery(seed, "Album art fetch settings");
    if (!query.has_value()) {
      return;
    }

    StoreAlbumArtSettings(*query);
    FetchAndSaveAlbumArt(data, *query);
  }

  GUID get_item_guid(unsigned index) override {
    if (index == 0) {
      return kAlbumArtMenuCommandGuid;
    }
    return pfc::guid_null;
  }

  bool get_item_description(unsigned index, pfc::string_base& out) override {
    if (index == 0) {
      out = "Search and save album art to selected files' folders";
      return true;
    }
    return false;
  }

  GUID get_parent() override { return contextmenu_groups::tagging; }
};

static contextmenu_item_factory_t<ContextTagLookup> g_context_tag_lookup_factory;
static contextmenu_item_factory_t<ContextAlbumArtFetch> g_context_album_art_fetch_factory;

DECLARE_COMPONENT_VERSION("Tag Lookup (mac starter)", "0.2.0",
                          "Looks up online release metadata and fetches album art for selected files.");

}  // namespace
