#import <AppKit/AppKit.h>

#include "match_selector.h"

#include <string>

@interface CoverPreviewController : NSObject
- (instancetype)initWithPopup:(NSPopUpButton*)popup
                     imageView:(NSImageView*)imageView
                    statusView:(NSTextField*)statusView
                     coverUrls:(NSArray<NSString*>*)coverUrls;
- (void)selectionChanged:(id)sender;
- (void)updatePreview;
@end

@implementation CoverPreviewController {
  NSPopUpButton* popup_;
  NSImageView* imageView_;
  NSTextField* statusView_;
  NSArray<NSString*>* coverUrls_;
}

- (instancetype)initWithPopup:(NSPopUpButton*)popup
                     imageView:(NSImageView*)imageView
                    statusView:(NSTextField*)statusView
                     coverUrls:(NSArray<NSString*>*)coverUrls {
  self = [super init];
  if (self != nil) {
    popup_ = popup;
    imageView_ = imageView;
    statusView_ = statusView;
    coverUrls_ = coverUrls;
  }
  return self;
}

- (void)selectionChanged:(id)sender {
  (void)sender;
  [self updatePreview];
}

- (void)updatePreview {
  const NSInteger index = popup_.indexOfSelectedItem;
  if (index < 0 || index >= static_cast<NSInteger>([coverUrls_ count])) {
    imageView_.image = nil;
    statusView_.stringValue = @"No cover selected.";
    return;
  }

  NSString* urlString = [coverUrls_ objectAtIndex:static_cast<NSUInteger>(index)];
  if (urlString == nil || [urlString length] == 0) {
    imageView_.image = nil;
    statusView_.stringValue = @"No cover available for this result.";
    return;
  }

  static NSMutableDictionary<NSString*, NSImage*>* cache = nil;
  if (cache == nil) {
    cache = [[NSMutableDictionary alloc] init];
  }

  NSImage* image = [cache objectForKey:urlString];
  if (image == nil) {
    NSURL* url = [NSURL URLWithString:urlString];
    if (url != nil) {
      NSData* data = [NSData dataWithContentsOfURL:url];
      if (data != nil && [data length] > 0) {
        image = [[NSImage alloc] initWithData:data];
        if (image != nil) {
          [cache setObject:image forKey:urlString];
        }
      }
    }
  }

  if (image != nil) {
    imageView_.image = image;
    statusView_.stringValue = @"Cover preview";
  } else {
    imageView_.image = nil;
    statusView_.stringValue = @"No cover available for this result.";
  }
}

@end

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
  NSString* releaseType = item.release_type.empty() ? @"" : [NSString stringWithFormat:@" [%@]",
                                                             ToNSString(item.release_type)];
  return [NSString stringWithFormat:@"%@ - %@ | %@ | %@ (%@)%@ [score %d]", artist, title, album,
                                    label, date, releaseType, item.score];
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

    NSView* container = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 760, 200)];

    NSPopUpButton* popup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 172, 760, 28)
                                                      pullsDown:NO];
    NSMutableArray<NSString*>* coverUrls = [NSMutableArray arrayWithCapacity:matches.size()];
    for (const auto& match : matches) {
      [popup addItemWithTitle:BuildItemLabel(match)];
      [coverUrls addObject:match.cover_url.empty() ? @"" : ToNSString(match.cover_url)];
    }
    [popup selectItemAtIndex:0];
    [container addSubview:popup];

    NSImageView* imageView = [[NSImageView alloc] initWithFrame:NSMakeRect(0, 8, 160, 160)];
    imageView.imageScaling = NSImageScaleProportionallyUpOrDown;
    imageView.imageFrameStyle = NSImageFrameGrayBezel;
    [container addSubview:imageView];

    NSTextField* statusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(170, 146, 580, 20)];
    statusLabel.editable = NO;
    statusLabel.bezeled = NO;
    statusLabel.drawsBackground = NO;
    statusLabel.selectable = NO;
    statusLabel.stringValue = @"Cover preview";
    [container addSubview:statusLabel];

    CoverPreviewController* previewController =
        [[CoverPreviewController alloc] initWithPopup:popup
                                            imageView:imageView
                                           statusView:statusLabel
                                            coverUrls:coverUrls];
    popup.target = previewController;
    popup.action = @selector(selectionChanged:);
    [previewController updatePreview];

    alert.accessoryView = container;

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
