# Tag Lookup Service - Requirements Specification

## Functional Requirements

### FR-001: Discogs API Integration
- **Description**: Must query Discogs API for release metadata
- **Input**: Album artist, album title, optional release year
- **Output**: Structured tracklist with metadata
- **Success Criteria**: Returns valid JSON with ≥80% field coverage

### FR-002: MusicBrainz API Integration
- **Description**: Must query MusicBrainz API for release metadata
- **Input**: Album artist, album title, release ID
- **Output**: Structured tracklist with artist credits and recordings
- **Success Criteria**: Returns valid JSON with artist credits and recording data

### FR-003: Credit Extraction
- **Description**: Must extract composer and performer credits from track data
- **Input**: Discogs track JSON with "credits" array OR MusicBrainz recording data
- **Output**: Comma-separated string of matching role names
- **Success Criteria**:
  - Correctly filters by role (composer, performer)
  - Handles multiple credits per track
  - Returns empty string when no matches
  - Case-insensitive role matching

### FR-004: Error Handling
- **Description**: Must handle API failures gracefully
- **Triggers**: Network errors, rate limiting, invalid responses
- **Behavior**: Return empty results with error logging
- **Recovery**: Implement exponential backoff for rate limits

### FR-005: Provider Fallback
- **Description**: Must support fallback between Discogs and MusicBrainz
- **Priority**: User-configurable provider order
- **Behavior**: Automatic fallback on API failure
- **Configuration**: Persistent provider preference

## Non-Functional Requirements

### NFR-001: Performance
- **Response Time**: <2s for cached requests, <5s for API calls
- **Memory Usage**: <50MB heap allocation per request
- **Concurrency**: Support 3 simultaneous lookups

### NFR-002: Data Validation
- **Input Validation**: Sanitize all API inputs
- **Output Validation**: Validate all tag values before propagation
- **Character Encoding**: Full Unicode support (UTF-8)

### NFR-003: Rate Limiting
- **Discogs API**: Max 60 requests/minute
- **MusicBrainz API**: Max 50 requests/second (with user-agent)
- **Retry Logic**: Exponential backoff (1s, 2s, 4s)
- **Cache**: 24-hour cache for successful responses

## Data Structures

### TrackInfo Structure
```
required fields:
- title (string, max 255 chars)
- artist (string, max 255 chars)
- trackNumber (string, max 10 chars)
- discNumber (string, max 5 chars)

optional fields:
- composer (string, max 512 chars)
- performer (string, max 512 chars)
- mediaType (string, max 50 chars)
- releaseId (string, max 36 chars)  # MusicBrainz release ID
- recordingId (string, max 36 chars) # MusicBrainz recording ID
```

### API Response Structures

#### Discogs Track Response
```
{
  "title": "string",
  "position": "string",
  "artists": [{"name": "string", "id": int}],
  "credits": [{"name": "string", "role": "string"}]
}
```

#### MusicBrainz Recording Response
```
{
  "title": "string",
  "position": int,
  "artist-credit": [{"artist": {"name": "string", "id": "uuid"}}],
  "id": "uuid"
}
```

## Test Coverage Requirements

### Unit Tests (100% coverage required)
- `ExtractDiscogsTrackCredits()` - all role matching scenarios
- JSON parsing edge cases (null, empty, malformed)
- Unicode handling in metadata fields
- MusicBrainz artist credit parsing
- MusicBrainz recording data extraction

### Integration Tests (95% coverage required)
- Full API request/response cycle (Discogs and MusicBrainz)
- Rate limiting behavior for both APIs
- Error condition handling
- Fallback between Discogs and MusicBrainz
