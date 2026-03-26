#import <AppKit/AppKit.h>

#include "album_art_selector.h"

#include <string>

@interface AlbumArtPreviewController : NSObject
- (instancetype)initWithPopup:(NSPopUpButton*)popup
                    imageView:(NSImageView*)imageView
                   statusView:(NSTextField*)statusView
                    coverUrls:(NSArray<NSString*>*)coverUrls;
- (void)selectionChanged:(id)sender;
- (void)updatePreview;
@end

@implementation AlbumArtPreviewController {
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
    statusView_.stringValue = @"No artwork selected.";
    return;
  }

  NSString* urlString = [coverUrls_ objectAtIndex:static_cast<NSUInteger>(index)];
  if (urlString == nil || [urlString length] == 0) {
    imageView_.image = nil;
    statusView_.stringValue = @"No preview available for this result.";
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
    statusView_.stringValue = @"Artwork preview";
  } else {
    imageView_.image = nil;
    statusView_.stringValue = @"No preview available for this result.";
  }
}

@end

namespace taglookup {
namespace {

NSString* ToNSString(const std::string& value) {
  return [NSString stringWithUTF8String:value.c_str()];
}

NSString* DisplayString(const std::string& value, NSString* fallback) {
  return value.empty() ? fallback : ToNSString(value);
}

NSString* BuildItemLabel(const AlbumArtCandidate& item) {
  NSString* artist = DisplayString(item.artist, @"?");
  NSString* album = DisplayString(item.album, @"Unknown release");
  NSString* label = DisplayString(item.label, @"Unknown label");
  NSString* date = DisplayString(item.date, @"Unknown date");
  NSString* provider = DisplayString(item.provider_hint, @"unknown");
  return [NSString stringWithFormat:@"%@ | %@ | %@ (%@) [%@]", artist, album, label, date,
                                    provider];
}

}  // namespace

std::optional<size_t> SelectAlbumArtCandidateIndex(const AlbumArtQuery& query,
                                                   const std::vector<AlbumArtCandidate>& matches) {
  if (matches.empty()) {
    return std::nullopt;
  }

  @autoreleasepool {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Select Album Art";

    NSString* artist = query.artist.empty() ? @"*" : ToNSString(query.artist);
    NSString* album = query.album.empty() ? @"*" : ToNSString(query.album);
    NSString* title = query.title.empty() ? @"*" : ToNSString(query.title);
    alert.informativeText =
        [NSString stringWithFormat:@"Found %lu artwork candidates for artist=%@, release=%@, track=%@.",
                                   (unsigned long)matches.size(), artist, album, title];

    [alert addButtonWithTitle:@"Use Selected Artwork"];
    [alert addButtonWithTitle:@"Cancel"];

    NSView* container = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 760, 200)];

    NSPopUpButton* popup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 172, 760, 28)
                                                      pullsDown:NO];
    NSMutableArray<NSString*>* coverUrls = [NSMutableArray arrayWithCapacity:matches.size()];
    for (const auto& match : matches) {
      [popup addItemWithTitle:BuildItemLabel(match)];
      [coverUrls addObject:match.url.empty() ? @"" : ToNSString(match.url)];
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
    statusLabel.stringValue = @"Artwork preview";
    [container addSubview:statusLabel];

    AlbumArtPreviewController* previewController =
        [[AlbumArtPreviewController alloc] initWithPopup:popup
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

    const NSInteger index = popup.indexOfSelectedItem;
    if (index < 0 || static_cast<size_t>(index) >= matches.size()) {
      return std::nullopt;
    }

    return static_cast<size_t>(index);
  }
}

}  // namespace taglookup
