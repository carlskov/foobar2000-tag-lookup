# fb2k Tag Lookup Component - Agent Documentation

## Overview
This document describes the autonomous agents and their capabilities in the fb2k tag lookup component codebase.

## Current Agents

### 1. Tag Lookup Service Agent
**Location**: `src/tag_lookup_service.cpp`
**Purpose**: Fetches album metadata from external sources (Discogs, MusicBrainz)

#### Capabilities:
- Query Discogs API for release information
- Query MusicBrainz API for release metadata
- Parse JSON responses and extract track metadata
- Handle rate limiting and error conditions
- Extract composer and performer credits from track data
- Fallback between providers on failure

#### Requirements:
- Must handle network failures gracefully
- Must respect API rate limits (Discogs: 60/min, MusicBrainz: 50/sec)
- Must validate and sanitize all external data
- Must support Unicode characters in metadata

### 2. Album Art Service Agent
**Location**: `src/album_art_service.cpp`
**Purpose**: Fetches and downloads album artwork from multiple providers

#### Capabilities:
- Search for album art across MusicBrainz, Discogs, and AlbumArtExchange
- Download image data from provider URLs
- Validate and cache downloaded images
- Rank results by relevance and quality
- Handle provider-specific API requirements

#### Requirements:
- Must support all three providers with proper authentication
- Must validate image format and quality
- Must implement caching with 24-hour expiration
- Must handle concurrent downloads efficiently

### 3. Tag Propagation Agent
**Location**: `src/component_main.cpp` (ReleaseTagPropagationFilter)
**Purpose**: Applies fetched metadata to audio files

#### Capabilities:
- Write standard tags (ARTIST, TITLE, ALBUM, etc.)
- Write extended tags (COMPOSER, PERFORMER)
- Handle multi-value fields (comma-separated)
- Preserve existing tags when appropriate

#### Requirements:
- Must not overwrite user-specified tags unless configured
- Must handle character encoding conversions
- Must validate tag values before writing
- Must support all common audio formats

### 4. Build/Packaging Agent
**Location**: `scripts/package_component.sh`
**Purpose**: Creates distributable component bundles

#### Capabilities:
- Generate versioned component bundles
- Auto-increment build numbers
- Code signing for macOS
- Create proper bundle structure

#### Requirements:
- Must maintain version consistency
- Must handle code signing failures
- Must create reproducible builds
- Must validate bundle structure

## Feature Specifications

Detailed requirements are maintained in the `specs/` directory:

```
specs/
├── tag_lookup_requirements.md      # Metadata fetching specifications
├── album_art_requirements.md      # Album art download specifications
├── tag_propagation_requirements.md  # Tag writing specifications  
├── build_requirements.md           # Packaging and distribution
└── testing_requirements.md         # Test coverage requirements
```

## Agent Interaction Flow

```
User Request → TagLookupService → External API → TagLookupService
                          ↓
                   ReleaseTagPropagationFilter → Audio Files
                          ↓
                     AlbumArtService → Image Providers → Downloaded Artwork
                          ↓
                     BuildAgent → Component Bundle
```

## Testing Agents

The test suite includes autonomous test agents that:
- Validate JSON parsing edge cases (tag_lookup_service_tests.cpp)
- Test credit extraction logic
- Verify tag propagation behavior
- Ensure build version consistency
- Test album art search and download workflows
- Validate image format handling and caching
- Verify provider fallback scenarios
