# Build System - Requirements Specification

## Functional Requirements

### FR-001: Version Management
- **Description**: Auto-increment build numbers
- **Input**: Base version from source code
- **Output**: Versioned component bundle
- **Format**: `MAJOR.MINOR.PATCH.BUILD`

### FR-002: Bundle Creation
- **Description**: Create macOS component bundle
- **Structure**:
  ```
  foo_taglookup.component/
  └── Contents/
      ├── Info.plist
      └── MacOS/
          └── foo_taglookup (binary)
  ```
- **Requirements**: Valid bundle identifier, proper permissions

### FR-003: Code Signing
- **Description**: Sign component bundle for macOS
- **Behavior**: Use ad-hoc signing if no certificate available
- **Validation**: Verify signature after signing

## Non-Functional Requirements

### NFR-001: Reproducibility
- **Requirement**: Identical output from identical inputs
- **Exceptions**: Build number, timestamps
- **Validation**: Checksum verification

### NFR-002: Performance
- **Build Time**: <30s for full build
- **Package Time**: <5s for bundle creation
- **Memory Usage**: <500MB peak

### NFR-003: Compatibility
- **macOS Versions**: 10.15+ (Catalina and later)
- **foobar2000**: v2.0+ component API
- **Architectures**: x86_64, arm64 (universal binary)

## Build Process

```
Input: Source code + dependencies
↓
Version Detection (from component_main.cpp)
↓
Build Number Increment (.build_version file)
↓
Compilation (C++17, clang)
↓
Bundle Creation (Info.plist generation)
↓
Code Signing
↓
Validation (bundle structure, signatures)
↓
Output: foo_taglookup.component
```

## Error Handling

### Error Conditions
- **E001**: Missing source files
- **E002**: Compilation failures
- **E003**: Code signing failures
- **E004**: Invalid bundle structure

### Recovery Behavior
- **E001/E002**: Fail fast with error message
- **E003**: Continue with unsigned bundle + warning
- **E004**: Clean and retry bundle creation

## Configuration Files

### .build_version
- **Format**: Single integer
- **Location**: Project root
- **Behavior**: Auto-created if missing, incremented on each build

### Info.plist Template
- **Required Fields**:
  - CFBundleIdentifier
  - CFBundleVersion
  - CFBundleShortVersionString
  - CFBundlePackageType

## Test Coverage Requirements

### Unit Tests (100% coverage)
- Version number parsing
- Bundle structure validation
- Info.plist generation

### Integration Tests (95% coverage)
- Full build pipeline
- Code signing verification
- Cross-architecture builds
