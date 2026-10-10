# Changelog

All notable changes to this extension are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Changed

- Output in single-byte encodings (Windows code pages, ISO 8859, KOI8, DOS
  code pages and so on) is converted about three times as fast.

### Fixed

- macOS: the output of a command that printed and exited at once (a task,
  for example) could be lost when VS Code read it late.
- Opening a second terminal of the same encoding from the dropdown while
  the first was still starting could leave the second one inactive.

## [0.1.0] - 2026-10-08

- Initial release.
