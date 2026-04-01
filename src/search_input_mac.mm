#import <AppKit/AppKit.h>

#include "search_input.h"

#include <string>

namespace taglookup {
namespace {

struct QueryPromptResult {
  int providerIndex = 0;
  std::string artist;
  std::string album;
  std::string label;
  std::string title;
  std::string year;
  SearchMode searchMode = SearchMode::ExactPhrase;
  bool overwriteTitle = false;
};

NSString* ToNSString(const std::string& value) {
  return [NSString stringWithUTF8String:value.c_str()];
}

std::string Trim(std::string s) {
  const size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

std::string FromNSString(NSString* value) {
  if (value == nil) {
    return "";
  }
  const char* utf8 = [value UTF8String];
  if (utf8 == nullptr) {
    return "";
  }
  return Trim(std::string(utf8));
}

NSTextField* MakeLabel(NSString* text, CGFloat y) {
  NSTextField* label = [[NSTextField alloc] initWithFrame:NSMakeRect(0, y, 110, 22)];
  label.stringValue = text;
  label.bezeled = NO;
  label.drawsBackground = NO;
  label.editable = NO;
  label.selectable = NO;
  return label;
}

NSTextField* MakeInput(NSString* value, CGFloat y) {
  NSTextField* input = [[NSTextField alloc] initWithFrame:NSMakeRect(118, y, 520, 24)];
  input.stringValue = value == nil ? @"" : value;
  return input;
}

std::optional<QueryPromptResult> RunQueryPrompt(NSString* messageText,
                                                NSString* informativeText,
                                                int providerIndex,
                                                const std::string& artist,
                                                const std::string& album,
                                                const std::string& label,
                                                const std::string& title,
                                                const std::string& year,
                                                SearchMode searchMode,
                                                bool showOverwriteTitle,
                                                bool overwriteTitle,
                                                const std::string& statusMessage) {
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = messageText;
  alert.informativeText = informativeText;

  [alert addButtonWithTitle:@"Search"];
  [alert addButtonWithTitle:@"Cancel"];

  const CGFloat viewHeight = showOverwriteTitle ? 310.0 : 282.0;
  NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 650, viewHeight)];

  NSPopUpButton* providerPopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(118, 252, 220, 24)
                                 pullsDown:NO];
  [providerPopup addItemWithTitle:@"MusicBrainz"];
  [providerPopup addItemWithTitle:@"Discogs"];
  [providerPopup addItemWithTitle:@"AlbumArtExchange"];
  [providerPopup selectItemAtIndex:providerIndex];

  NSTextField* artistInput = MakeInput(ToNSString(artist), 220);
  NSTextField* titleInput = MakeInput(ToNSString(title), 188);
  NSTextField* albumInput = MakeInput(ToNSString(album), 156);
  NSTextField* labelInput = MakeInput(ToNSString(label), 124);
  NSTextField* yearInput = MakeInput(ToNSString(year), 92);

  NSButton* tokenizedToggle = [[NSButton alloc] initWithFrame:NSMakeRect(118, 56, 520, 24)];
  [tokenizedToggle setButtonType:NSButtonTypeSwitch];
  tokenizedToggle.title = @"Use tokenized broad search (uncheck for exact phrase search)";
  tokenizedToggle.state = (searchMode == SearchMode::Tokenized) ? NSControlStateValueOn
                                 : NSControlStateValueOff;

  NSButton* overwriteTitleToggle = nil;
  if (showOverwriteTitle) {
    overwriteTitleToggle = [[NSButton alloc] initWithFrame:NSMakeRect(118, 28, 520, 24)];
    [overwriteTitleToggle setButtonType:NSButtonTypeSwitch];
    overwriteTitleToggle.title = @"When applying to selection, overwrite TITLE on all files";
    overwriteTitleToggle.state = overwriteTitle ? NSControlStateValueOn : NSControlStateValueOff;
  }

  const CGFloat statusY = showOverwriteTitle ? 2.0 : 28.0;
  NSTextField* statusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(118, statusY, 520, 24)];
  statusLabel.bezeled = NO;
  statusLabel.drawsBackground = NO;
  statusLabel.editable = NO;
  statusLabel.selectable = NO;
  statusLabel.textColor = [NSColor systemRedColor];
  statusLabel.font = [NSFont systemFontOfSize:12.0];
  statusLabel.stringValue = statusMessage.empty() ? @"" : ToNSString(statusMessage);

  [view addSubview:MakeLabel(@"Provider", 254)];
  [view addSubview:MakeLabel(@"Artist", 222)];
  [view addSubview:MakeLabel(@"Track", 190)];
  [view addSubview:MakeLabel(@"Release", 158)];
  [view addSubview:MakeLabel(@"Label", 126)];
  [view addSubview:MakeLabel(@"Year", 94)];
  [view addSubview:providerPopup];
  [view addSubview:artistInput];
  [view addSubview:titleInput];
  [view addSubview:albumInput];
  [view addSubview:labelInput];
  [view addSubview:yearInput];
  [view addSubview:tokenizedToggle];
  if (overwriteTitleToggle != nil) {
    [view addSubview:overwriteTitleToggle];
  }
  [view addSubview:statusLabel];

  alert.accessoryView = view;

  const NSModalResponse response = [alert runModal];
  if (response != NSAlertFirstButtonReturn) {
    return std::nullopt;
  }

  QueryPromptResult result;
  result.providerIndex = static_cast<int>(providerPopup.indexOfSelectedItem);
  result.artist = FromNSString(artistInput.stringValue);
  result.album = FromNSString(albumInput.stringValue);
  result.label = FromNSString(labelInput.stringValue);
  result.title = FromNSString(titleInput.stringValue);
  result.year = FromNSString(yearInput.stringValue);
  result.searchMode = (tokenizedToggle.state == NSControlStateValueOn) ? SearchMode::Tokenized
                                                                       : SearchMode::ExactPhrase;
  result.overwriteTitle = overwriteTitleToggle != nil &&
                          overwriteTitleToggle.state == NSControlStateValueOn;

  if (result.artist.empty() && result.album.empty() && result.label.empty() &&
      result.title.empty() && result.year.empty()) {
    return std::nullopt;
  }

  return result;
}

}  // namespace

std::optional<LookupQuery> PromptForLookupQuery(const LookupQuery& seed,
                                                const std::string& statusMessage) {
  @autoreleasepool {
    const auto result = RunQueryPrompt(@"Tag Lookup Query",
                                       @"Fill any fields. Results must match every field you provide.",
                                       (seed.provider == LookupProvider::Discogs) ? 1 : 0,
                                       seed.artist,
                                       seed.album,
                                       seed.label,
                                       seed.title,
                                       seed.year,
                                       seed.search_mode,
                                       true,
                                       seed.overwrite_title_on_propagation,
                                       statusMessage);
    if (!result.has_value()) {
      return std::nullopt;
    }

    LookupQuery query;
    query.artist = result->artist;
    query.album = result->album;
    query.label = result->label;
    query.title = result->title;
    query.year = result->year;
    query.search_mode = result->searchMode;
    query.overwrite_title_on_propagation = result->overwriteTitle;
    query.provider = (result->providerIndex == 1) ? LookupProvider::Discogs
                                                  : LookupProvider::MusicBrainz;

    return query;
  }
}

std::optional<AlbumArtQuery> PromptForAlbumArtQuery(const AlbumArtQuery& seed,
                                                    const std::string& statusMessage) {
  @autoreleasepool {
    const auto result = RunQueryPrompt(@"Album Art Query",
                                       @"Fill any fields. Results must match the provider query you provide.",
                                       (seed.provider == AlbumArtProvider::Discogs) ? 1 : 0,
                                       seed.artist,
                                       seed.album,
                                       seed.label,
                                       seed.title,
                                       seed.year,
                                       seed.search_mode,
                                       false,
                                       false,
                                       statusMessage);
    if (!result.has_value()) {
      return std::nullopt;
    }

    AlbumArtQuery query;
    query.artist = result->artist;
    query.album = result->album;
    query.label = result->label;
    query.title = result->title;
    query.year = result->year;
    query.search_mode = result->searchMode;
    query.provider = (result->providerIndex == 1) ? AlbumArtProvider::Discogs
                                    : (result->providerIndex == 2) ? AlbumArtProvider::AlbumArtExchange
                                                                  : AlbumArtProvider::MusicBrainz;

    return query;
  }
}

}  // namespace taglookup
