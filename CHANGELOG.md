# Change Log
All notable changes to Sylvan will be documented in this file.

## [Unreleased]
### Added
- This CHANGELOG file.
- Custom leaves can now implement custom callbacks for writing/reading to/from files.
- Implemented GMP leaf writing/reading to/from file.

### Changed
- The API to register a custom MTBDD leaf now requires multiple calls, which is better design for future extensions.
