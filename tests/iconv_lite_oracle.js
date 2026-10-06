// What VS Code's editor does with each byte sequence and character, for
// tests/test_editor_parity.py: VS Code decodes and encodes files with
// iconv-lite, as @vscode/iconv-lite-umd, the package and version
// (package-lock.json) used here.
//
//   node tests/iconv_lite_oracle.js list
//     -> JSON [{ id, luitEncoding }] from src/encodings.ts
//   node tests/iconv_lite_oracle.js decode <id>   (stdin: one hex byte sequence per line)
//     -> per line, the decoded code points in hex, space-separated
//   node tests/iconv_lite_oracle.js encode <id>   (stdin: one hex code point per line)
//     -> per line, the encoded bytes in hex, or "-" if the editor can't encode it
"use strict";

const fs = require("fs");
const path = require("path");
const iconv = require("@vscode/iconv-lite-umd");

function readLines() {
  const text = fs.readFileSync(0, "utf8");
  const lines = text.split("\n");
  if (lines[lines.length - 1] === "") lines.pop();
  return lines;
}

const [command, id] = process.argv.slice(2);
if (command === "list") {
  // Node 24 strips the TypeScript types itself.
  const { ENCODINGS } = require(
    path.join(__dirname, "..", "src", "encodings.ts"),
  );
  process.stdout.write(
    JSON.stringify(
      ENCODINGS.map((e) => ({ id: e.id, luitEncoding: e.luitEncoding })),
    ),
  );
} else if (command === "decode") {
  const out = readLines().map((hex) =>
    [...iconv.decode(Buffer.from(hex, "hex"), id)]
      .map((c) => c.codePointAt(0).toString(16))
      .join(" "),
  );
  process.stdout.write(out.join("\n") + "\n");
} else if (command === "encode") {
  const out = readLines().map((hex) => {
    const ch = String.fromCodePoint(parseInt(hex, 16));
    const bytes = Buffer.from(iconv.encode(ch, id));
    // iconv-lite substitutes "?" for what it can't encode
    return iconv.decode(bytes, id) === ch ? bytes.toString("hex") : "-";
  });
  process.stdout.write(out.join("\n") + "\n");
} else {
  process.stderr.write(
    "usage: iconv_lite_oracle.js list | decode <id> | encode <id>\n",
  );
  process.exit(2);
}
