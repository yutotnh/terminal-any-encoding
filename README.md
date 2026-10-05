# Terminal Any Encoding

[日本語版はこちら / Japanese version](README.ja.md)

Use non-UTF-8 encodings in the VS Code integrated terminal.
All 45 non-Unicode encodings VS Code supports are available, including EUC-JP, Shift JIS, GBK, Big5-HKSCS and EUC-KR.

![A Japanese (EUC-JP) terminal showing EUC-JP file names and contents, with the terminal dropdown listing this extension's 🌐 profiles](images/screenshot.png)

## Requirements

VS Code 1.73.0 or later.
Supported operating systems (for a remote connection, the remote host's):

| OS      | Support                                   |
| ------- | ----------------------------------------- |
| Linux   | x64 / arm64 / armhf (Alpine: x64 / arm64) |
| macOS   | x64 / arm64                               |
| Windows | Not supported                             |

## Getting Started

Pick an encoding from `🌐 Select Encoding...` in the ▼ next to the `+` button in the terminal panel.

To skip the picker:

- set [`terminalAnyEncoding.defaultEncoding`](#settings) and pick `🌐 Default Encoding`,
- bind a [key](#keybindings), or
- [make it your default terminal](#using-this-extension-as-your-default-terminal).

## Commands

| Command                            | In the dropdown         | Description                                                                         |
| ---------------------------------- | ----------------------- | ----------------------------------------------------------------------------------- |
| `Open Terminal with Encoding...`   | `🌐 Select Encoding...` | Shows the encoding picker. Passing an id (below) as the argument opens it directly. |
| `Open Terminal (Default Encoding)` | `🌐 Default Encoding`   | Opens a terminal with `defaultEncoding`, or shows the picker when it's empty.       |

An id is the same as VS Code's `files.encoding` value (`eucjp`, `shiftjis`, etc.) and is shown next to each label in the picker.

### Keybindings

Pass an id to `terminalAnyEncoding.openTerminal` to open it with one key:

```json
[
  {
    "key": "ctrl+alt+e",
    "command": "terminalAnyEncoding.openTerminal",
    "args": "eucjp"
  },
  {
    "key": "ctrl+alt+s",
    "command": "terminalAnyEncoding.openTerminal",
    "args": "shiftjis"
  }
]
```

## Settings

| Setting                                           | Description                                                                                                                             |
| ------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| `terminalAnyEncoding.defaultEncoding`             | Encoding used by `🌐 Default Encoding`. Empty (the default) shows the picker each time.                                                 |
| `terminalAnyEncoding.shellProfile.linux` / `.osx` | Profile (a name from `terminal.integrated.profiles.*`) of the shell the terminal starts. Empty (the default) uses your default profile. |
| `terminalAnyEncoding.warnAboutLocale`             | Warn when no matching locale is available (default `true`).                                                                             |

### Using this extension as your default terminal

Set `terminal.integrated.defaultProfile.linux` (`.osx` on macOS) to `🌐 Default Encoding`, and "New Terminal" (Ctrl+Shift+\`) opens this extension's terminal.

```json
{
  "terminalAnyEncoding.defaultEncoding": "eucjp",
  "terminal.integrated.defaultProfile.linux": "🌐 Default Encoding"
}
```

In this case, set the shell to start with `shellProfile.*`.
Without it, `$SHELL` is used (as a login shell on macOS).

```json
{
  "terminalAnyEncoding.shellProfile.linux": "zsh"
}
```

## Locale Detection

Each terminal gets `LANG` set to a locale matching its encoding (e.g. `ja_JP.EUC-JP`).
If no matching locale is available, a warning tells you what to do.

If `~/.bashrc` or similar sets `LANG` or `LC_ALL`, it overrides this.
Programs that handle text by locale, like `date` and `ls`, then display it wrong without any warning (`ls` shows an EUC-JP file name like `$'\244\242'`).
Programs that print file contents as-is, like `cat`, aren't affected.
Keep a value that's already set instead: `export LANG="${LANG:-ja_JP.UTF-8}"`.

## Tasks

A task of type `terminalAnyEncoding` has its input and output converted (problem matchers read the converted output too).
Write it like a `shell` task, plus an `encoding` (an id):

```json
{
  "version": "2.0.0",
  "tasks": [
    {
      "label": "build",
      "type": "terminalAnyEncoding",
      "encoding": "eucjp",
      "command": "make",
      "args": ["all"],
      "problemMatcher": "$gcc"
    }
  ]
}
```

- The command line (including values of variables like `${file}`) is interpreted in the task's encoding.
  So files with non-ASCII UTF-8 names (such as ones created in VS Code) aren't found.
- A command line with a character the encoding can't represent isn't run, and the task fails.

## Shell Integration

Works for bash, zsh, fish and pwsh, as in VS Code's own terminals.

## Known Limitations and Troubleshooting

- **Input with a character the encoding can't represent (e.g. an emoji in EUC-JP) isn't sent.**
  A notification names the character.
- **Tab names stay fixed on macOS, e.g. `bash (EUC-JP)`.**
- **Mojibake or `�`.**
  Check the selected encoding. If it's right, check the locale with `locale charmap`.
- **Output of other task types and of debug sessions isn't converted.**
  For a task, use the [`terminalAnyEncoding` type](#tasks).
  Debugging has no equivalent.
- **The wave dash looks different from VS Code's editor.**
  The editor shows Shift JIS `81 60` and EUC-JP `A1 C1` as a fullwidth tilde (U+FF5E); this extension shows a wave dash (U+301C).
  Either can be typed.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

MIT.
See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for bundled third-party licenses.
