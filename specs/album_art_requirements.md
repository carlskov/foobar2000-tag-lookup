# Album Art Service - Requirements Specification

## Overview
The Album Art Service provides functionality to search for and download album artwork from multiple providers.

## Functional Requirements

### FR-001: Multi-Provider Support
- **Description**: Must support multiple album art providers
- **Supported Providers**:
  - MusicBrainz (Cover Art Archive)
  - Discogs
  - AlbumArtExchange
- **Provider Selection**: User-configurable with persistent storage

### FR-002: Album Art Search
- **Description**: Must search for album art based on metadata
- **Input**: AlbumArtQuery containing artist, album, label, year
- **Output**: Vector of AlbumArtCandidate results
- **Success Criteria**: Returns ≥1 valid image URL for known albums

### FR-003: Image Download
- **Description**: Must download image data from URLs
- **Input**: Image URL
- **Output**: Raw image bytes and content type
- **Success Criteria**: Valid image data with correct content type

### FR-004: MusicBrainz Cover Art Archive Integration
- **Description**: Must fetch album art from MusicBrainz Cover Art Archive
- **Endpoint**: `https://coverartarchive.org/release/{mbid}`
- **Input**: MusicBrainz Release ID
- **Output**: Vector of image URLs with type information
- **Image Types**: Front, back, booklet
- **Size Preferences**: 500px preferred, fallback to 250px or thumbnail
- **Authentication**: None required
- **Rate Limit**: 50 requests/second

### FR-005: Result Ranking
- **Description**: Must rank search results by relevance
- **Ranking Factors**:
  - Exact match priority
  - Provider reliability score
  - Image resolution (higher = better)
  - File size (larger = better, within reason)

### FR-006: Caching
- **Description**: Must cache downloaded images
- **Cache Duration**: 24 hours for successful downloads
- **Cache Size**: Max 100MB disk usage
- **Cache Key**: URL + last-modified header

## Non-Functional Requirements

### NFR-001: Performance
- **Search Time**: <1.5s per provider
- **Download Time**: <3s for typical images (500KB)
- **Concurrency**: Support 2 simultaneous downloads
- **Memory Usage**: <50MB peak during operations

### NFR-002: Image Quality
- **Minimum Resolution**: 300x300 pixels
- **Preferred Resolution**: 500x500+ pixels
- **Format Support**: JPEG, PNG, WebP
- **Color Depth**: 24-bit minimum

### NFR-003: Network Efficiency
- **Connection Reuse**: HTTP keep-alive
- **Compression**: Accept gzip/deflate
- **Timeout**: 10s for search, 30s for download
- **Retry Logic**: 2 retries for failed requests

### NFR-004: Error Handling
- **Network Failures**: Graceful degradation
- **Invalid Images**: Discard corrupt downloads
- **Rate Limiting**: Respect provider limits
- **Fallback**: Try alternative providers on failure

## Data Structures

### AlbumArtQuery
```
required fields:
- artist (string, max 255 chars)
- album (string, max 255 chars)

optional fields:
- label (string, max 100 chars)
- year (string, 4 digits)
- title (string, max 255 chars)
- search_mode (enum: ExactPhrase, AllWords, AnyWords)
- provider (enum: MusicBrainz, Discogs, AlbumArtExchange)
```

### AlbumArtCandidate
```
required fields:
- url (string, valid HTTP/HTTPS URL)
- artist (string, max 255 chars)
- album (string, max 255 chars)

optional fields:
- label (string, max 100 chars)
- date (string, YYYY or YYYY-MM-DD)
- provider_hint (string, provider name)
- extension_hint (string, file extension)
```

### Provider-Specific Response Formats

#### MusicBrainz (Cover Art Archive)
```json
{
  "images": [
    {
      "types": ["front"],
      "front": true,
      "back": false,
      "thumbnails": {
        "500": "https://...",
        "250": "https://..."
      },
      "image": "https://..."
    }
  ]
}
```

#### Discogs
```json
{
  "images": [
    {
      "type": "primary",
      "uri": "https://...",
      "uri150": "https://..."
    }
  ]
}
```

#### AlbumArtExchange
```json
{
  "result": "success",
  "image": "https://...",
  "thumbnail": "https://..."
}
```

## API Endpoints

### MusicBrainz Cover Art Archive
- **Base URL**: `https://coverartarchive.org`
- **Release Endpoint**: `/release/{mbid}`
- **Search Endpoint**: `/release/?q={query}`
- **Authentication**: None required
- **Rate Limit**: 50 requests/second

### Discogs API
- **Base URL**: `https://api.discogs.com`
- **Search Endpoint**: `/database/search`
- **Release Endpoint**: `/releases/{id}`
- **Authentication**: Required (user token)
- **Rate Limit**: 60 requests/minute

### AlbumArtExchange API
- **Base URL**: `https://www.albumartexchange.com`
- **Search Endpoint**: `/api/search.php`
- **Authentication**: None required
- **Rate Limit**: No explicit limit (be polite)

## Test Coverage Requirements

### Unit Tests (100% coverage)
- Provider-specific URL building
- JSON response parsing for all providers
- Image URL extraction logic
- Content type detection
- File extension guessing

### Integration Tests (95% coverage)
- Full search workflow for each provider
- Image download and validation
- Error condition handling
- Provider fallback scenarios
- Cache behavior verification

### Test Data Requirements
- Sample API responses for all providers
- Valid and invalid image URLs
- Various image formats (JPEG, PNG, WebP)
- Rate limit simulation responses

## Error Conditions & Recovery

### EC-001: Network Failure
- **Detection**: Connection timeout or refusal
- **Recovery**: Retry 2 times, then fallback to next provider

### EC-002: Invalid API Response
- **Detection**: Malformed JSON or unexpected structure
- **Recovery**: Log error, skip to next provider

### EC-003: Image Download Failure
- **Detection**: HTTP 4xx/5xx, corrupt data
- **Recovery**: Remove from results, try next candidate

### EC-004: Rate Limiting
- **Detection**: HTTP 429 or rate limit headers
- **Recovery**: Wait specified retry-after time

### EC-005: No Results Found
- **Detection**: Empty result set
- **Recovery**: Return empty vector (no error)

## Provider Comparison Matrix

| Provider          | Coverage | Quality | Speed | Auth Required | Rate Limit       |
|-------------------|----------|---------|-------|---------------|------------------|
| MusicBrainz       | High     | High    | Fast  | No            | 50 req/s         |
| Discogs           | Very High| Very High| Medium| Yes (token)   | 60 req/min       |
| AlbumArtExchange  | Medium   | Medium  | Fast  | No            | Be polite        |

## Implementation Notes

### Provider Selection Logic
1. Check user preference setting
2. Try preferred provider first
3. On failure, fallback to next provider in order
4. Return first successful result set

### Image Validation
- Verify content type matches extension
- Check minimum dimensions (300x300)
- Validate image can be decoded
- Reject corrupt or invalid images

### Performance Optimization
- Parallel provider queries (when appropriate)
- Early termination on first good result
- Connection pooling and reuse
- Caching of search results and images
