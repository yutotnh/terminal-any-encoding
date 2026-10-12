# Changelog

All notable changes to this extension are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.1.1] - 2026-10-12

### Fixed

- Pasted or typed non-ASCII text could reach the shell as something else:
  - Part of a paste with a character the encoding can't represent could
    still get through, over a remote connection for example, and leave
    the shell taking what you typed next as pasted. In a shell that uses
    bracketed paste, such a paste is now dropped from that character on
    (a local paste is dropped whole, as VS Code sends it at once). To
    recognize where a paste starts, the Escape key pressed alone now
    reaches the program up to 10 ms later.
  - A character typed with Alt, or as the next key after the Escape key,
    was sent as a different character (あ as ã), or garbled instead of
    rejected when the encoding can't represent it.
- macOS: the output of a command that prints and exits at once, such as a
  task, could be lost.
- Output in single-byte encodings (Windows code pages, ISO 8859, KOI8, DOS
  code pages and so on) was slow to show: it's now two to four times as
  fast.
- Opening two terminals of the same encoding from the dropdown in quick
  succession could bring the first one to the front instead of the second.

## [0.1.0] - 2026-10-08

- Initial release.
