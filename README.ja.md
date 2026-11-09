# Terminal Any Encoding

[English version here](README.md)

VS Code の統合ターミナルで、UTF-8 以外の文字エンコーディングを使えるようにする拡張機能です。

VS Code の「エンコード付きで再度開く」に並ぶ Unicode 以外のエンコーディング 45 種類すべてに対応しています。西欧、中欧、キリル文字、ギリシャ語、トルコ語、アラビア語、ヘブライ語、バルト語、タイ語、ベトナム語から、中国語、日本語、韓国語までを使えます。古い VS Code の一覧にない CP 1125 と CP 857 も、バージョンによらず使えます。

[Visual Studio Marketplace](https://marketplace.visualstudio.com/items?itemName=yutotnh.terminal-any-encoding) または [Open VSX](https://open-vsx.org/extension/yutotnh/terminal-any-encoding) からインストールできます。

![EUC-JP のファイル名と内容を表示する Japanese (EUC-JP) ターミナルと、この拡張機能の 🌐 プロファイルが並ぶターミナルドロップダウン](images/screenshot.png)

## 動作要件

VS Code 1.73.0 以降が必要です。対応 OS は次のとおりです（リモート接続では接続先の OS）。

| OS      | 対応                                              |
| ------- | ------------------------------------------------- |
| Linux   | x64、arm64、armhf（Alpine Linux は x64 と arm64） |
| macOS   | x64、arm64                                        |
| Windows | 非対応                                            |

## 使い方

コマンドラインからは、次のコマンドでインストールできます。

```sh
code --install-extension yutotnh.terminal-any-encoding
```

ターミナルパネルの `+` の横にある ▼ から `🌐 Select Encoding...` を選び、エンコーディングを選ぶと、そのエンコーディングのターミナルが開きます。

毎回選ぶのを省くには、次のいずれかの方法を使います。

- [`terminalAnyEncoding.defaultEncoding`](#設定) にエンコーディングを設定し、`🌐 Default Encoding` を選ぶ
- 特定のエンコーディングでターミナルを開く[キーバインディング](#キーバインディング)を登録する
- [この拡張機能のターミナルを既定のターミナルにする](#この拡張機能のターミナルを既定のターミナルにする)

## コマンド

| コマンド                                      | ドロップダウンでの表示  | 動作                                                                                        |
| --------------------------------------------- | ----------------------- | ------------------------------------------------------------------------------------------- |
| エンコーディングを指定してターミナルを開く... | `🌐 Select Encoding...` | エンコーディングの選択画面を表示します。引数に id（後述）を渡すと、選択画面を経ずに開きます |
| 既定のエンコーディングでターミナルを開く      | `🌐 Default Encoding`   | `defaultEncoding` のエンコーディングで開きます。空欄なら選択画面を表示します                |

id は VS Code の設定 `files.encoding` に指定する値（`eucjp`、`shiftjis` など）と同じで、選択画面では各エンコーディング名の横に表示されます。

### キーバインディング

`terminalAnyEncoding.openTerminal` の引数に id を渡すと、1 回のキー操作でそのエンコーディングのターミナルを開けます。`keybindings.json` の例:

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

| 設定                                              | 内容                                                                                                                                            |
| ------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| `terminalAnyEncoding.defaultEncoding`             | `🌐 Default Encoding` で使うエンコーディング。空欄（既定値）なら毎回選択画面を表示します                                                        |
| `terminalAnyEncoding.shellProfile.linux` / `.osx` | ターミナル内で起動するシェルのプロファイル名（`terminal.integrated.profiles.*` で定義した名前）。空欄（既定値）なら既定のプロファイルを使います |
| `terminalAnyEncoding.warnAboutLocale`             | エンコーディングに合うロケールがないときに警告するかどうか。既定値は `true`                                                                     |

### この拡張機能のターミナルを既定のターミナルにする

`terminal.integrated.defaultProfile.linux`（macOS では `.osx`）を `🌐 Default Encoding` にすると、「新しいターミナル」（Ctrl+Shift+\`）でこの拡張機能のターミナルが開きます。

```json
{
  "terminalAnyEncoding.defaultEncoding": "eucjp",
  "terminal.integrated.defaultProfile.linux": "🌐 Default Encoding"
}
```

このとき起動するシェルは `shellProfile.*` で指定します。指定しなければ `$SHELL`（macOS ではログインシェル）が起動します。

```json
{
  "terminalAnyEncoding.shellProfile.linux": "zsh"
}
```

## ロケールの自動設定

ターミナルを開くと、エンコーディングに合うロケールを `LANG` に設定します（EUC-JP なら `ja_JP.EUC-JP` など）。これで `date` や `ls` のような、ロケールに従って文字を扱うプログラムも、そのエンコーディングで出力します。

合うロケールがホストにない場合は、生成すべきロケールを通知で知らせます。この通知は「今後表示しない」か、設定の [`warnAboutLocale`](#設定) で止められます。

ただし、次のエンコーディングには Linux の glibc が用意するロケールがそもそもなく、生成もできないため、通知は出ません。

- Shift JIS（ディストリビューションが `ja_JP.sjis` などを追加している場合を除く）
- Windows 874
- DOS のコードページ（CP 437、850、852、857、865、866）

これらでは `LANG` が VS Code から引き継いだ値のままになるため、`date` や `ls` の表示が崩れることがあります。

### シェルの設定ファイルで `LANG` を設定している場合

`~/.bashrc` などで `LANG` や `LC_ALL` を設定していると、この拡張機能が設定した値が上書きされ、`date` や `ls` の表示が崩れます。たとえば `ls` は、EUC-JP のファイル名を `$'\244\242'` のように表示します。上書きはシェルの起動後に起こるため、通知も出ません。`cat` のようにファイルの中身をそのまま出力するプログラムには影響しません。

`LANG` が未設定のときだけ値を設定するように書けば、この問題は起きません。

```sh
export LANG="${LANG:-ja_JP.UTF-8}"
```

## タスク

`tasks.json` で `terminalAnyEncoding` タイプのタスクを定義すると、そのタスクの入出力も変換されます（problem matcher も変換後の出力を解析します）。書き方は `shell` タイプと同じで、`encoding` に id を指定します。

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

- コマンドラインは、`${file}` などの変数を展開した値も含めて、タスクのエンコーディングで解釈されます。そのため、名前に ASCII 以外の文字を含む UTF-8 のファイル（VS Code で作ったファイルなど）は見つかりません。
- エンコーディングで表せない文字を含むコマンドラインは実行されず、タスクは失敗します。

## シェル統合

bash、zsh、fish、pwsh では、通常のターミナルと同じようにシェル統合が使えます。

## 既知の制限とトラブルシューティング

### 入力した文字が送信されない

エンコーディングで表せない文字（EUC-JP のターミナルでの絵文字など）を含む入力は送信されません。どの文字が原因かは通知で知らせます。

貼り付けた文字列にこうした文字があった場合は、その貼り付けの残りも送信されません。対象は、シェルがブラケットペーストを使うなら貼り付けの終わりまで、使わないなら入力が途切れるまでで、いずれも最大 2 秒です（遅いリモート接続などで 2 秒より後に届いた分は送信されます）。

### 文字化けする、`�` が表示される

まず、ターミナルのエンコーディングが、扱っているファイルやプログラムのエンコーディングと合っているか確認してください。合っている場合は、ターミナルで `locale charmap` を実行してロケールのエンコーディングを確認してください（[ロケールの自動設定](#ロケールの自動設定)を参照）。

### macOS でタブ名が変わらない

macOS では、タブ名が開いたときの名前（`bash (EUC-JP)` など）のまま変わりません。

### ほかのタイプのタスクやデバッグの出力が変換されない

変換されるのは、この拡張機能のターミナルと `terminalAnyEncoding` タイプのタスクだけです。タスクは [`terminalAnyEncoding` タイプ](#タスク)に書き換えれば変換されます。デバッグの出力は変換できません。

### EUC-JP で入力した `～` と `№` のバイト列が、エディタで保存したものと異なる

EUC-JP のターミナルで入力した `～` と `№` は、それぞれ `A1 C1` と `AD E2` として送信します。ターミナルではこのバイト列がそのまま `～` と `№` として表示され、wave-dash-unify 拡張機能が保存するバイト列とも一致します。一方、VS Code のエディタだけで保存すると `8F A2 B7` と `8F A2 F1` になり、ほかのツールでは読めません。

`〜`（U+301C）も波ダッシュとして、EUC-JP では `A1 C1`、Shift JIS では `81 60` を送信します。

## 開発

[CONTRIBUTING.md](CONTRIBUTING.md)（英語）を参照してください。

## ライセンス

MIT ライセンスです。同梱しているソフトウェアのライセンスは [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) を参照してください。
