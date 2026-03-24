#include <foobar2000/SDK/foobar2000.h>

#include "match_selector.h"
#include "search_input.h"
#include "tag_lookup_service.h"

#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr GUID kMenuCommandGuid =
    {0x7fc620ce, 0x9fd4, 0x4f7f, {0xab, 0x8e, 0x43, 0xac, 0x03, 0x68, 0x52, 0xb2}};

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

      pfc::string_formatter msg;
      msg << "Selected " << static_cast<unsigned>(*selectedIndex + 1) << " of "
          << static_cast<unsigned>(matches.size()) << " matches\n\n";
      msg << "Artist: " << result.artist.c_str() << "\n";
      msg << "Title: " << result.title.c_str() << "\n";
      msg << "Album: " << result.album.c_str() << "\n";
      msg << "Label: " << result.label.c_str() << "\n";
      msg << "Date: " << result.date.c_str() << "\n";
      msg << "Score: " << result.score << "\n\n";
      msg << "Next step: write these values back through metadb_io_v2."
             " This starter keeps writes disabled by default.";

      popup_message::g_show(msg, "Tag Lookup Result");
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

DECLARE_COMPONENT_VERSION("Tag Lookup (mac starter)", "0.1.0", "Looks up tags from MusicBrainz for selected track.");

}  // namespace
