#import <AppKit/AppKit.h>

#include "match_selector.h"

#include <string>

namespace taglookup {
namespace {

NSString* ToNSString(const std::string& value) {
  return [NSString stringWithUTF8String:value.c_str()];
}

NSString* BuildItemLabel(const TagResult& item) {
  NSString* artist = item.artist.empty() ? @"?" : ToNSString(item.artist);
  NSString* title = item.title.empty() ? @"?" : ToNSString(item.title);
  NSString* album = item.album.empty() ? @"Unknown album" : ToNSString(item.album);
  NSString* label = item.label.empty() ? @"Unknown label" : ToNSString(item.label);
  NSString* date = item.date.empty() ? @"Unknown date" : ToNSString(item.date);
  return [NSString stringWithFormat:@"%@ - %@ | %@ | %@ (%@) [score %d]", artist, title, album,
                                    label, date, item.score];
}

}  // namespace

std::optional<size_t> SelectTagResultIndex(const LookupQuery& query,
                                           const std::vector<TagResult>& matches) {
  if (matches.empty()) {
    return std::nullopt;
  }

  @autoreleasepool {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Select Release";

    NSString* artist = query.artist.empty() ? @"*" : ToNSString(query.artist);
    NSString* title = query.title.empty() ? @"*" : ToNSString(query.title);
    NSString* album = query.album.empty() ? @"*" : ToNSString(query.album);
    NSString* label = query.label.empty() ? @"*" : ToNSString(query.label);
    NSString* year = query.year.empty() ? @"*" : ToNSString(query.year);
    alert.informativeText =
      [NSString stringWithFormat:@"Found %lu matches for artist=%@, release=%@, label=%@, track=%@, year=%@.",
                     (unsigned long)matches.size(), artist, album, label, title, year];

    [alert addButtonWithTitle:@"Use Selected Match"];
    [alert addButtonWithTitle:@"Cancel"];

    NSPopUpButton* popup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 760, 28)
                                                      pullsDown:NO];
    for (const auto& match : matches) {
      [popup addItemWithTitle:BuildItemLabel(match)];
    }
    [popup selectItemAtIndex:0];

    alert.accessoryView = popup;

    const NSModalResponse response = [alert runModal];
    if (response != NSAlertFirstButtonReturn) {
      return std::nullopt;
    }

    NSInteger index = popup.indexOfSelectedItem;
    if (index < 0 || static_cast<size_t>(index) >= matches.size()) {
      return std::nullopt;
    }

    return static_cast<size_t>(index);
  }
}

}  // namespace taglookup
