#include <foobar2000/SDK/foobar2000.h>

#include "match_selector.h"
#include "search_input.h"
#include "tag_lookup_service.h"

#include <array>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr GUID kMenuCommandGuid =
    {0x7fc620ce, 0x9fd4, 0x4f7f, {0xab, 0x8e, 0x43, 0xac, 0x03, 0x68, 0x52, 0xb2}};
constexpr GUID kSearchProviderSettingGuid =
    {0xc1138cbc, 0x6553, 0x4b14, {0xa6, 0xd7, 0x7e, 0x22, 0xc9, 0xde, 0x39, 0x20}};
constexpr GUID kSearchModeSettingGuid =
    {0x02a030e9, 0x2ca3, 0x4a9d, {0x83, 0x3e, 0x49, 0x0f, 0x66, 0xd5, 0xbe, 0xb2}};
constexpr GUID kOverwriteTitleSettingGuid =
    {0x09d1d2fc, 0x9e24, 0x4528, {0xbc, 0xb8, 0x53, 0x8e, 0x9d, 0x63, 0x54, 0x71}};

cfg_int g_searchProviderSetting(kSearchProviderSettingGuid,
                                static_cast<int>(taglookup::SearchProvider::MusicBrainz));
cfg_int g_searchModeSetting(kSearchModeSettingGuid,
                            static_cast<int>(taglookup::SearchMode::ExactPhrase));
cfg_bool g_overwriteTitleSetting(kOverwriteTitleSettingGuid, false);

std::string Trim(std::string s) {
  const size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

bool ParseArtistTitle(const std::string& value, taglookup::LookupQuery& out) {
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

bool TryBuildQueryFromFilename(const metadb_handle_ptr& track, taglookup::LookupQuery& out) {
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
                              const taglookup::TagResult& result,
                              std::vector<taglookup::TrackInfo> tracks,
                              bool overwriteTitle)
      : album_artist_(std::move(albumArtist)), total_tracks_(std::move(totalTracks)),
        total_discs_(std::move(totalDiscs)), genre_(std::move(genre)), result_(result),
        tracks_(std::move(tracks)), overwrite_title_(overwriteTitle) {
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
    setIfNonEmpty("LABEL", result_.label);
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
                              const taglookup::TagResult& result,
                              const std::vector<taglookup::TrackInfo>& tracks,
                              bool overwriteTitle) {
  const auto wndParent = core_api::get_main_window();
  auto filter = fb2k::service_new<ReleaseTagPropagationFilter>(data, albumArtist, totalTracks,
                                                               totalDiscs, genre, result, tracks,
                                                               overwriteTitle);

  auto notify = fb2k::makeCompletionNotify([](unsigned code) {
    FB2K_console_formatter() << "Tag propagation finished, code: " << code;
  });

  const uint32_t flags = metadb_io_v2::op_flag_partial_info_aware;
  metadb_io_v2::get()->update_info_async(data, filter, wndParent, flags, notify);
}

taglookup::SearchProvider LoadSearchProviderSetting() {
  const int value = static_cast<int>(g_searchProviderSetting);
  if (value == static_cast<int>(taglookup::SearchProvider::Discogs)) {
    return taglookup::SearchProvider::Discogs;
  }
  return taglookup::SearchProvider::MusicBrainz;
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
    std::string statusMessage;
    taglookup::LookupQuery query;
    std::vector<taglookup::TagResult> matches;

    while (true) {
      const auto queryOpt = taglookup::PromptForLookupQuery(seed, statusMessage);
      if (!queryOpt.has_value()) {
        return;
      }

      query = *queryOpt;
      StoreLookupSettings(query);
      seed = query;
      matches = service.LookupAll(query, 50);

      if (matches.empty()) {
        statusMessage = "No results found. Adjust fields and try again.";
        continue;
      }

      const auto selectedIndex = taglookup::SelectTagResultIndex(query, matches);
      if (!selectedIndex.has_value()) {
        statusMessage = "Selection canceled. You can refine and search again.";
        continue;
      }

      const auto& result = matches[*selectedIndex];

      // Fetch the full, ordered tracklist for the selected release in one API call.
      // This gives us per-track title and artist so each selected file gets its own tags.
      const taglookup::TracklistResult tracklistResult = service.FetchTracklist(query, result);
      const std::string albumArtist =
          tracklistResult.albumArtist.empty() ? result.artist : tracklistResult.albumArtist;

        PropagateTagsToSelection(data, albumArtist, tracklistResult.totalTracks,
                     tracklistResult.totalDiscs, tracklistResult.genre,
                     result, tracklistResult.tracks,
                               query.overwrite_title_on_propagation);

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

static contextmenu_item_factory_t<ContextTagLookup> g_context_tag_lookup_factory;

DECLARE_COMPONENT_VERSION("Tag Lookup (mac starter)", "0.1.9",
                          "Looks up online release metadata and propagates tags to selected files.");

}  // namespace
