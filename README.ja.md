# Terminal Any Encoding

[English version here](README.md)

[Visual Studio Marketplace](https://marketplace.visualstudio.com/items?itemName=yutotnh.terminal-any-encoding) または [Open VSX](https://open-vsx.org/extension/yutotnh/terminal-any-encoding) からインストールできる。

VS Code の統合ターミナルで、UTF-8 以外の文字エンコーディングを使えるようにする拡張機能である。

対象は、VS Code が扱える Unicode 以外のエンコーディング 45 種類すべてであり、西欧、中欧、キリル文字、ギリシャ語、トルコ語、アラビア語、ヘブライ語、バルト語、タイ語、ベトナム語から、中国語、日本語、韓国語までを含む。この 45 種類は、VS Code の「エンコード付きで再度開く」の一覧のうち、Unicode 以外のものと一致する。古い VS Code の一覧には CP 1125 と CP 857 がないが、この拡張機能では VS Code のバージョンによらずこの 2 つも使える。

![EUC-JP のファイル名と内容を表示する Japanese (EUC-JP) ターミナルと、この拡張機能の 🌐 プロファイルが並ぶターミナルドロップダウン](images/screenshot.png)

## 動作要件

VS Code 1.73.0 以降が必要である。対応 OS は次のとおりで、リモート接続で使う場合は接続先の OS が対象となる。

| OS      | 対応                                              |
| ------- | ------------------------------------------------- |
| Linux   | x64、arm64、armhf（Alpine Linux は x64 と arm64） |
| macOS   | x64、arm64                                        |
| Windows | 非対応                                            |

## 使い方

コマンドラインからインストールする場合は、次のコマンドを実行する。

```sh
code --install-extension yutotnh.terminal-any-encoding
```

ターミナルパネルの `+` の横にある ▼ から `🌐 Select Encoding...` を選び、続けてエンコーディングを選ぶと、そのエンコーディングで動作するターミナルが開く。

同じエンコーディングを繰り返し使う場合は、次のいずれかの方法で選択を省略できる。

- [`terminalAnyEncoding.defaultEncoding`](#設定) にエンコーディングを設定し、`🌐 Default Encoding` を選ぶ
- 特定のエンコーディングでターミナルを開く[キーバインディング](#キーバインディング)を登録する
- [この拡張機能のターミナルを既定のターミナルにする](#この拡張機能のターミナルを既定のターミナルにする)

## コマンド

| コマンド                                      | ドロップダウンでの表示  | 動作                                                                                                        |
| --------------------------------------------- | ----------------------- | ----------------------------------------------------------------------------------------------------------- |
| エンコーディングを指定してターミナルを開く... | `🌐 Select Encoding...` | エンコーディングの選択画面を表示する。引数に id（後述）を渡すと、選択画面を経ずにそのエンコーディングで開く |
| 既定のエンコーディングでターミナルを開く      | `🌐 Default Encoding`   | `defaultEncoding` に設定したエンコーディングで開く。空欄の場合は選択画面を表示する                          |

id は、VS Code の設定 `files.encoding` に指定する値と同じもの（`eucjp`、`shiftjis` など）で、選択画面では各エンコーディング名の横に表示される。

### キーバインディング

`terminalAnyEncoding.openTerminal` の引数に id を渡すキーバインディングを登録すると、1 回のキー操作でそのエンコーディングのターミナルを開ける。`keybindings.json` での記述例を示す。

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

## 設定

| 設定                                              | 内容                                                                                                                                                        |
| ------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `terminalAnyEncoding.defaultEncoding`             | `🌐 Default Encoding` で使うエンコーディング。空欄（既定値）の場合は、毎回選択画面を表示する                                                                |
| `terminalAnyEncoding.shellProfile.linux` / `.osx` | ターミナル内で起動するシェルのプロファイル名（`terminal.integrated.profiles.*` で定義した名前）。空欄（既定値）の場合は、VS Code の既定のプロファイルを使う |
| `terminalAnyEncoding.warnAboutLocale`             | エンコーディングに合うロケールを使えないときに警告するかどうか。既定値は `true`                                                                             |

### この拡張機能のターミナルを既定のターミナルにする

`terminal.integrated.defaultProfile.linux`（macOS では `.osx`）を `🌐 Default Encoding` にすると、「新しいターミナル」（Ctrl+Shift+\`）でこの拡張機能のターミナルが開くようになる。

```json
{
  "terminalAnyEncoding.defaultEncoding": "eucjp",
  "terminal.integrated.defaultProfile.linux": "🌐 Default Encoding"
}
```

この場合、ターミナル内で起動するシェルは `shellProfile.*` で指定する。指定がなければ `$SHELL`（macOS ではログインシェル）を起動する。

```json
{
  "terminalAnyEncoding.shellProfile.linux": "zsh"
}
```

## ロケールの自動設定

ターミナルを開くたびに、選んだエンコーディングに合うロケールを環境変数 `LANG` に設定する（EUC-JP なら `ja_JP.EUC-JP` など）。`date` や `ls` のように、ロケールに従って文字を扱うプログラムが、そのエンコーディングで出力するようにするためである。

合うロケールがホストにインストールされていない場合は、警告を通知し、生成すべきロケールを示す。この警告は、通知の「今後表示しない」か、設定の [`warnAboutLocale`](#設定) で無効にできる。

ただし、次のエンコーディングについては、Linux の glibc が標準で用意するロケール自体が存在しない。生成による対処ができないため、警告も表示しない。

- Shift JIS（ディストリビューションが `ja_JP.sjis` のようなロケールを追加している場合を除く）
- Windows 874
- DOS のコードページ（CP 437、850、852、857、865、866）

これらのエンコーディングでは、`LANG` は VS Code から引き継いだ値のままとなるため、`date` や `ls` の表示が崩れることがある。

### シェルの設定ファイルで `LANG` を設定している場合

`~/.bashrc` などで `LANG` や `LC_ALL` を設定していると、この拡張機能が設定した値が上書きされ、`date` や `ls` の表示が崩れる。たとえば `ls` は、EUC-JP のファイル名を `$'\244\242'` のように表示する。上書きはシェルの起動後に行われるため、警告は表示されない。なお、`cat` のようにファイルの中身をそのまま出力するプログラムは影響を受けない。

これを避けるには、既に設定されている値を残す書き方にする。

```sh
export LANG="${LANG:-ja_JP.UTF-8}"
```

## タスク

`tasks.json` で `terminalAnyEncoding` タイプのタスクを定義すると、そのタスクの入出力もエンコーディングの変換対象となる。problem matcher が解析するのも変換後の出力である。書き方は `shell` タイプのタスクと同じで、`encoding` に id を加える。

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

コマンドラインは、`${file}` などの変数を展開した値も含めて、タスクのエンコーディングで解釈される。そのため、VS Code で作成したファイルのように、名前に ASCII 以外の文字を含む UTF-8 のファイルは見つからない。また、そのエンコーディングで表せない文字を含むコマンドラインは実行されず、タスクは失敗する。

## シェル統合

bash、zsh、fish、pwsh では、通常のターミナルと同様にシェル統合が機能する。

## 既知の制限とトラブルシューティング

### 入力した文字が送信されない

エンコーディングで表せない文字（EUC-JP のターミナルにおける絵文字など）を含む入力は、ターミナルに送信されない。原因となった文字は通知で示す。貼り付けた文字列に含まれていた場合は、その貼り付けの残りも送信されない。ただし、2 秒より遅れて届いた残り（遅いリモート接続で起こりうる）は送信される。

### 文字化けする、`�` が表示される

まず、ターミナルのエンコーディングが、扱っているファイルやプログラムのエンコーディングと一致しているかを確認する。一致している場合は、ターミナルで `locale charmap` を実行し、ロケールのエンコーディングを確認する（[ロケールの自動設定](#ロケールの自動設定)を参照）。

### macOS でタブ名が変わらない

macOS では、タブ名が `bash (EUC-JP)` のまま変化しない。

### ほかのタイプのタスクやデバッグの出力が変換されない

変換の対象は、この拡張機能のターミナルと `terminalAnyEncoding` タイプのタスクに限られる。タスクは [`terminalAnyEncoding` タイプ](#タスク)に書き換えれば変換される。デバッグの出力を変換する手段はない。

### EUC-JP で入力した `～` と `№` のバイト列が、エディタで保存したものと異なる

EUC-JP のターミナルで入力した `～` と `№` は、`A1 C1` と `AD E2` として送信する。ターミナルはこの 2 つのバイト列を `～` と `№` として表示するため、表示される文字と入力した文字のバイト列が一致する。これは wave-dash-unify 拡張機能が保存するバイト列とも同じである。一方、VS Code のエディタ単体で保存すると `8F A2 B7` と `8F A2 F1` になり、ほかのツールでは読めない。

`〜`（U+301C）も波ダッシュとして送信する。そのバイト列は、EUC-JP では `A1 C1`、Shift JIS では `81 60` である。

## 開発

[CONTRIBUTING.md](CONTRIBUTING.md)（英語）を参照。

## ライセンス

MIT ライセンス。同梱しているソフトウェアのライセンスは [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) を参照。
