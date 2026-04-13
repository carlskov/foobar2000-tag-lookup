# Tag Lookup Service - Requirements Specification

## Functional Requirements

### FR-001: Multi-Field Search Interface
- **Description**: Must support flexible search using multiple metadata fields
- **Input Fields**:
  - artist (string, optional)
  - album (string, optional) 
  - label (string, optional)
  - title (string, optional)
  - year (string, optional)
  - search_mode (enum: ExactPhrase, AllWords, AnyWords)
  - provider (enum: MusicBrainz, Discogs)
- **Validation**: At least one field must be non-empty
- **Output**: Vector of TagResult matches (up to configurable limit)

### FR-002: MusicBrainz Search API
- **Description**: Must search MusicBrainz database using multiple fields
- **Endpoint**: `https://musicbrainz.org/ws/2/release/`
- **Query Parameters**:
  - artist: Exact or partial match
  - release: Album title match
  - label: Label name match
  - recording: Track title match (if provided)
  - date: Year match (if provided)
- **Search Modes**:
  - ExactPhrase: Full phrase matching
  - AllWords: All words must match
  - AnyWords: Any word may match
- **Pagination**: 100 results per page, multiple pages as needed
- **Output**: Vector of TagResult with score, release ID, and basic metadata

### FR-003: Discogs Search API
- **Description**: Must search Discogs database with master release priority
- **Endpoint**: `https://api.discogs.com/database/search`
- **Search Strategy**:
  1. **Primary Search**: Master releases first (`type:master`)
  2. **Fallback Search**: Specific releases if master search yields no results
  3. **Track-Level Search**: If title provided, search with track information
- **Query Parameters**:
  - artist: Artist name
  - release_title: Album title
  - label: Label name
  - track: Track title (when provided)
  - year: Release year
  - type: "master" for primary search, omitted for fallback
- **Pagination**: Configurable page size, multiple pages as needed
- **Master Release Resolution**:
  - When master ID found, fetch master release JSON
  - Extract main release ID from master
  - Use master cover art as fallback
- **Result Processing**:
  - Parse "Artist - Album" format from title field
  - Extract release type (master vs specific)
  - Handle both master_id and release_id appropriately

### FR-003: Credit Extraction
- **Description**: Must extract composer and performer credits from track data
- **Input**: Discogs track JSON with "credits" array OR MusicBrainz recording data
- **Output**: Comma-separated string of matching role names
- **Success Criteria**:
  - Correctly filters by role (composer, performer)
  - Handles multiple credits per track
  - Returns empty string when no matches
  - Case-insensitive role matching

### FR-004: Search Result Processing
- **Description**: Must process and filter search results
- **Features**:
  - Deduplication: Remove duplicate releases
  - Scoring: Assign relevance scores based on match quality
  - Limiting: Respect user-specified result limit (max 50)
  - Track Matching: For track-level searches, find matching track titles
- **Track Matching Logic**:
  - When title field provided, search recordings/tracks
  - Return only releases containing matching track
  - Extract exact track title from release data

### FR-005: Error Handling
- **Description**: Must handle API failures gracefully
- **Triggers**: Network errors, rate limiting, invalid responses, empty results
- **Behavior**: 
  - Return empty vector on failure
  - Log errors to console/stderr
  - No exceptions thrown to callers
- **Validation**:
  - Check for empty input fields
  - Validate JSON response structure
  - Handle missing or null fields gracefully

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
- Discogs master release fallback logic
- JSON parsing edge cases (null, empty, malformed)
- Unicode handling in metadata fields
- MusicBrainz artist credit parsing from release JSON
- MusicBrainz recording data extraction

### Integration Tests (95% coverage required)
- Full API request/response cycle (Discogs and MusicBrainz)
- Rate limiting behavior for both APIs
- Error condition handling
- Discogs master release fallback scenarios
