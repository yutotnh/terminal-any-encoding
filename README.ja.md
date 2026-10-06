# Terminal Any Encoding

[English version here](README.md)

VS Code の統合ターミナルで UTF-8 以外の文字エンコーディングを使えるようにする拡張機能。
EUC-JP、Shift JIS、GBK、Big5-HKSCS、EUC-KR など、VS Code が対応する Unicode 以外のエンコーディング 45 種類すべてに対応する。

![EUC-JP のファイル名と内容を表示する Japanese (EUC-JP) ターミナルと、この拡張の 🌐 プロファイルが並ぶターミナルドロップダウン](images/screenshot.png)

## 動作要件

VS Code 1.73.0 以降。
対応 OS は次のとおり(リモート接続時は接続先の OS)。

| OS      | 対応                                       |
| ------- | ------------------------------------------ |
| Linux   | x64 / arm64 / armhf(Alpine は x64 / arm64) |
| macOS   | x64 / arm64                                |
| Windows | 非対応                                     |

## 使い方

ターミナルパネルの `+` の横の ▼ の `🌐 Select Encoding...` からエンコーディングを選ぶ。

毎回の選択を省くには、次のどれかを使う。

- [`terminalAnyEncoding.defaultEncoding`](#設定) を設定し、`🌐 Default Encoding` を選ぶ。
- [キーバインディング](#キーバインディング)を登録する。
- [この拡張を既定のターミナルにする](#この拡張を既定のターミナルにする)。

## コマンド

| コマンド                                      | ドロップダウンの表示    | 説明                                                                        |
| --------------------------------------------- | ----------------------- | --------------------------------------------------------------------------- |
| エンコーディングを指定してターミナルを開く... | `🌐 Select Encoding...` | 選択画面を表示する。引数に id(下記)を渡すと、そのエンコーディングで直接開く |
| 既定のエンコーディングでターミナルを開く      | `🌐 Default Encoding`   | `defaultEncoding` のエンコーディングで開く。空欄なら選択画面を表示する      |

id は VS Code の `files.encoding` の値と同じ(`eucjp`、`shiftjis` など)で、選択画面のラベルの横に表示される。

### キーバインディング

`terminalAnyEncoding.openTerminal` の引数に id を渡すと、1 キーで開ける。

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

| 設定                                              | 説明                                                                                                           |
| ------------------------------------------------- | -------------------------------------------------------------------------------------------------------------- |
| `terminalAnyEncoding.defaultEncoding`             | `🌐 Default Encoding` で使うエンコーディング。空欄(既定)なら毎回選択画面を表示する                             |
| `terminalAnyEncoding.shellProfile.linux` / `.osx` | ターミナルで起動するシェルのプロファイル名(`terminal.integrated.profiles.*`)。空欄(既定)なら既定のプロファイル |
| `terminalAnyEncoding.warnAboutLocale`             | 合うロケールを使えないときに警告する(既定 `true`)                                                              |

### この拡張を既定のターミナルにする

`terminal.integrated.defaultProfile.linux`(macOS は `.osx`)を `🌐 Default Encoding` にすると、「新しいターミナル」(Ctrl+Shift+\`)でこの拡張のターミナルが開く。

```json
{
  "terminalAnyEncoding.defaultEncoding": "eucjp",
  "terminal.integrated.defaultProfile.linux": "🌐 Default Encoding"
}
```

この場合、起動するシェルは `shellProfile.*` で指定する。
指定しなければ `$SHELL`(macOS ではログインシェル)を使う。

```json
{
  "terminalAnyEncoding.shellProfile.linux": "zsh"
}
```

## ロケールの自動設定

ターミナルを開くたびに、選んだエンコーディングに合うロケールを `LANG` に設定する(例: `ja_JP.EUC-JP`)。
合うロケールを使えないときは、警告で対処を示す。

`~/.bashrc` などで `LANG` や `LC_ALL` を設定すると、この設定が上書きされる。
すると `date` や `ls` など、ロケールに従って文字を扱うプログラムの表示が、警告なしに崩れる(`ls` は EUC-JP のファイル名を `$'\244\242'` のように表示する)。
ファイルの中身をそのまま表示する `cat` などは影響を受けない。
既存の値を残す書き方にする: `export LANG="${LANG:-ja_JP.UTF-8}"`。

## タスク

`tasks.json` の `terminalAnyEncoding` タイプのタスクは、入出力が変換される(problem matcher も変換後の出力を読む)。
書き方は `shell` タスクに `encoding`(id)を加えたもの:

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

- コマンドライン(`${file}` などの変数の値も含む)は、タスクのエンコーディングで解釈される。
  そのため、名前に ASCII 以外の文字を含む UTF-8 のファイル(VS Code で作ったものなど)は見つからない。
- エンコーディングで表せない文字を含むコマンドラインは実行せず、タスクは失敗する。

## シェル統合

bash、zsh、fish、pwsh で通常のターミナルと同じく使える。

## 既知の制限とトラブルシューティング

- **エンコーディングで表せない文字(EUC-JP での絵文字など)を含む入力は送らない。**
  原因の文字は通知で示す。
- **macOS ではタブ名が `bash (EUC-JP)` のまま変わらない。**
- **文字化けする、`�` が出る。**
  エンコーディングの選択を確認し、正しければ `locale charmap` でロケールを確認する。
- **ほかのタイプのタスクとデバッグの出力は変換されない。**
  タスクは [`terminalAnyEncoding` タイプ](#タスク)にすれば変換される。
  デバッグには手段がない。
- **EUC-JP で入力した `～` と `№` は、VS Code のエディタが保存するバイト列と違う。**
  表示の元になったバイト列 `A1 C1` と `AD E2` で送る。wave-dash-unify 拡張が保存するのと同じである。
  エディタ単体では `8F A2 B7` と `8F A2 F1` で保存され、ほかのツールでは読めない。
  `〜`(U+301C)も波ダッシュとして送る。EUC-JP では `A1 C1`、Shift JIS では `81 60` である。

## 開発

[CONTRIBUTING.md](CONTRIBUTING.md)(英語)を参照。

## ライセンス

MIT。
同梱物のライセンスは [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) を参照。
