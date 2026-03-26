# foo_taglookup (Foobar2000 macOS starter component)

This is a Foobar2000 component project for macOS that adds these context-menu commands:

- `Lookup Tags Online`
- `Fetch Album Art`

The command currently:

1. Opens a query dialog with fields: Provider, Artist, Track, Release, Label, Year.
2. Defaults to exact phrase search and lets you optionally toggle tokenized (broad) search.
3. Prefills Artist + Track from clipboard (`Artist - Title`) or filename when possible.
4. Calls the selected provider web API and fetches multiple possible matches.
5. Enforces that every field you filled out must match returned data.
6. Shows a selectable list of releases.
7. Applies the selected release metadata to all selected files.
8. Can optionally overwrite TITLE for all selected files when enabled in the search dialog.

The album art command currently:

1. Opens a query dialog with fields: Provider, Artist, Track, Release, Label, Year.
2. Remembers its own provider and search-mode settings independently from tag lookup.
3. Searches MusicBrainz or Discogs for artwork candidates.
4. Shows a selectable list of artwork candidates with a preview.
5. Downloads the selected image and saves it as `cover.<ext>` in each unique folder containing the selected files.

Tag propagation currently applies release-level fields across the current selection: artist, album, date, label, and release IDs. It does not force `ALBUM ARTIST`. For Discogs, the component resolves track artist from the selected release details when available. Per-file titles are preserved unless you enable the overwrite-title option.

The dialog remembers the last used provider, search mode, and title-overwrite setting. Search text fields are not persisted.

## What is included

- C++20 component skeleton (`src/component_main.cpp`)
- Web lookup service with `libcurl` + JSON parsing (`src/tag_lookup_service.cpp`)
- Album art lookup + download service (`src/album_art_service.cpp`)
- Album art candidate picker (`src/album_art_selector_mac.mm`)
- CMake project setup for Xcode generation (`CMakeLists.txt`)

## Prerequisites

- Xcode / Command Line Tools
- CMake 3.21+
- foobar2000 SDK extracted locally
- Internet access for metadata queries

## Build (macOS)

```bash
cmake -S . -B build
cmake --build build --config Release
```

If you have full Xcode installed and want an Xcode project:

```bash
cmake -S . -B build-xcode -G Xcode
cmake --build build-xcode --config Release
```

## Package as .component (macOS)

After building, create an installable bundle:

```bash
./scripts/package_component.sh
```

This creates:

- `dist/foo_taglookup.component`

Install it by copying to:

- `~/Library/Application Support/foobar2000-v2/components`

## Run tests

After configuring the project, run:

```bash
cmake --build build --target tag_lookup_service_tests album_art_service_tests
ctest --test-dir build --output-on-failure
```

The tests cover the provider-specific URL building and parsing helpers used by the tag lookup and album art services.

If your SDK is not at `sdk/foobar2000-sdk-*`, pass it explicitly:

```bash
cmake -S . -B build -G Xcode -DFOOBAR2000_SDK_ROOT=/absolute/path/to/foobar2000-sdk
```

## Important SDK linkage note

The current `CMakeLists.txt` builds required foobar2000 SDK static libraries from your local SDK source tree and links them automatically.

## Recommended next steps

1. Replace filename parsing with real tag extraction from `file_info`.
2. Add confidence scoring and candidate disambiguation UI.
3. Improve writeback safeguards and track-level mapping behavior.
4. Add request throttling, retries, and local cache.
5. Add opt-in providers (Discogs, MusicBrainz release group, etc.).

## Manual search usage

When a file is badly named, you can still search with the dialog fields.

1. Optionally copy text in the form `Artist - Title` to clipboard for prefill.
2. Right-click the track and run `Lookup Tags Online`.
3. Enter any subset of Artist, Release, Track, Year.
4. Exact phrase search is the default. Enable tokenized search only when you want broader matching.
5. Enable the title overwrite checkbox only if you want all selected files to get the same TITLE value.

Every field you provide is treated as required for candidate matching.

## Album art usage

1. Right-click one or more tracks and run `Fetch Album Art`.
2. Enter any subset of Artist, Release, Track, Label, Year.
3. Choose MusicBrainz or Discogs and optionally broaden the search with tokenized mode.
4. Pick the artwork candidate you want from the preview list.
5. The component writes `cover.jpg`, `cover.png`, or `cover.webp` into each selected folder.

If multiple selected files are in the same folder, that folder is written once.

## Legal / API usage

- Follow MusicBrainz API usage policy and include a real contact URL in the user-agent.
- Respect provider terms, rate limits, and attribution requirements.
