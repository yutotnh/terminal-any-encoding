# Changelog

All notable changes to this extension are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Changed

- Output in single-byte encodings (Windows code pages, ISO 8859, KOI8, DOS
  code pages and so on) is converted two to four times as fast, depending
  on the text.

### Fixed

- macOS: the output of a command that printed and exited at once (a task,
  for example) could be lost when VS Code read it late.
- Opening a second terminal of the same encoding from the dropdown while
  the first was still starting could leave the second one inactive.
- When a paste was rejected for a character the encoding can't represent,
  the rest of it could still reach the shell if it arrived more than
  50 ms later, as it can over a remote connection, and a paste's end
  marker split across reads could leave the shell in paste mode. The rest
  of a paste is now dropped up to its end (for 2 seconds at most), and
  input after the paste's end is sent as usual.

## [0.1.0] - 2026-10-08

- Initial release.
