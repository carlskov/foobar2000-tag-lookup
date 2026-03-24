# foo_taglookup (Foobar2000 macOS starter component)

This is a starter Foobar2000 component project for macOS that adds a context-menu command:

- `Lookup Tags Online`

The command currently:

1. Opens a query dialog with fields: Provider, Artist, Track, Release, Label, Year.
2. Lets you toggle search mode: tokenized (broad) or exact phrase (strict).
3. Prefills Artist + Track from clipboard (`Artist - Title`) or filename when possible.
4. Calls MusicBrainz web API and fetches multiple possible matches.
5. Enforces that every field you filled out must match returned data.
6. Shows a selectable list of releases.
7. Uses your selected release and shows the chosen tags in a popup.

Writing tags back to files is intentionally left disabled in this starter so you can choose your preferred write strategy and safeguards first.

## What is included

- C++20 component skeleton (`src/component_main.cpp`)
- Web lookup service with `libcurl` + JSON parsing (`src/tag_lookup_service.cpp`)
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

If your SDK is not at `sdk/foobar2000-sdk-*`, pass it explicitly:

```bash
cmake -S . -B build -G Xcode -DFOOBAR2000_SDK_ROOT=/absolute/path/to/foobar2000-sdk
```

## Important SDK linkage note

The current `CMakeLists.txt` builds required foobar2000 SDK static libraries from your local SDK source tree and links them automatically.

## Recommended next steps

1. Replace filename parsing with real tag extraction from `file_info`.
2. Add confidence scoring and candidate disambiguation UI.
3. Implement writeback via `metadb_io_v2` with undo-friendly behavior.
4. Add request throttling, retries, and local cache.
5. Add opt-in providers (Discogs, MusicBrainz release group, etc.).

## Manual search usage

When a file is badly named, you can still search with the dialog fields.

1. Optionally copy text in the form `Artist - Title` to clipboard for prefill.
2. Right-click the track and run `Lookup Tags Online`.
3. Enter any subset of Artist, Release, Track, Year.
4. Choose tokenized search for broader results, or exact phrase for tighter results.

Every field you provide is treated as required for candidate matching.

## Legal / API usage

- Follow MusicBrainz API usage policy and include a real contact URL in the user-agent.
- Respect provider terms, rate limits, and attribution requirements.
