#import <AppKit/AppKit.h>

#include "search_input.h"

#include <string>

namespace taglookup {
namespace {

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

}  // namespace

std::optional<LookupQuery> PromptForLookupQuery(const LookupQuery& seed,
                                                const std::string& statusMessage) {
  @autoreleasepool {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Tag Lookup Query";
    alert.informativeText = @"Fill any fields. Results must match every field you provide.";

    [alert addButtonWithTitle:@"Search"];
    [alert addButtonWithTitle:@"Cancel"];

    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 650, 310)];

    NSPopUpButton* providerPopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(118, 252, 220, 24)
                                   pullsDown:NO];
    [providerPopup addItemWithTitle:@"MusicBrainz"];
    [providerPopup addItemWithTitle:@"Discogs"];
    [providerPopup selectItemAtIndex:(seed.provider == SearchProvider::Discogs) ? 1 : 0];

    NSTextField* artistInput = MakeInput(ToNSString(seed.artist), 220);
    NSTextField* titleInput = MakeInput(ToNSString(seed.title), 188);
    NSTextField* albumInput = MakeInput(ToNSString(seed.album), 156);
    NSTextField* labelInput = MakeInput(ToNSString(seed.label), 124);
    NSTextField* yearInput = MakeInput(ToNSString(seed.year), 92);

    NSButton* tokenizedToggle = [[NSButton alloc] initWithFrame:NSMakeRect(118, 56, 520, 24)];
    [tokenizedToggle setButtonType:NSButtonTypeSwitch];
    tokenizedToggle.title = @"Use tokenized broad search (uncheck for exact phrase search)";
    tokenizedToggle.state = (seed.search_mode == SearchMode::Tokenized) ? NSControlStateValueOn
                                       : NSControlStateValueOff;

    NSButton* overwriteTitleToggle = [[NSButton alloc] initWithFrame:NSMakeRect(118, 28, 520, 24)];
    [overwriteTitleToggle setButtonType:NSButtonTypeSwitch];
    overwriteTitleToggle.title = @"When applying to selection, overwrite TITLE on all files";
    overwriteTitleToggle.state = seed.overwrite_title_on_propagation ? NSControlStateValueOn
                                      : NSControlStateValueOff;

    NSTextField* statusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(118, 2, 520, 24)];
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
    [view addSubview:overwriteTitleToggle];
    [view addSubview:statusLabel];

    alert.accessoryView = view;

    const NSModalResponse response = [alert runModal];
    if (response != NSAlertFirstButtonReturn) {
      return std::nullopt;
    }

    LookupQuery query;
    query.artist = FromNSString(artistInput.stringValue);
    query.album = FromNSString(albumInput.stringValue);
    query.label = FromNSString(labelInput.stringValue);
    query.title = FromNSString(titleInput.stringValue);
    query.year = FromNSString(yearInput.stringValue);
    query.search_mode = (tokenizedToggle.state == NSControlStateValueOn) ? SearchMode::Tokenized
                                        : SearchMode::ExactPhrase;
    query.overwrite_title_on_propagation = (overwriteTitleToggle.state == NSControlStateValueOn);
    query.provider = (providerPopup.indexOfSelectedItem == 1) ? SearchProvider::Discogs
                                  : SearchProvider::MusicBrainz;

    if (query.artist.empty() && query.album.empty() && query.label.empty() &&
        query.title.empty() && query.year.empty()) {
      return std::nullopt;
    }

    return query;
  }
}

}  // namespace taglookup
