// iconv-lite, as tools/gen-tables/gen_tables.py uses it: VS Code decodes and
// encodes files with this library (as @vscode/iconv-lite-umd, the package
// and version used here), so tables built from it make the terminal show
// what the editor shows.
//
//   node iconv_lite.js version
//   node iconv_lite.js decode <encoding>   (stdin: one hex byte sequence per line)
//     -> per line, the decoded code points in hex, space-separated
//   node iconv_lite.js encode <encoding>   (stdin: one hex code point per line)
//     -> per line, the encoded bytes in hex, or "-" if it can't be encoded
"use strict";

const fs = require("fs");
const iconv = require("@vscode/iconv-lite-umd");

const [command, encoding] = process.argv.slice(2);
if (command === "version") {
  process.stdout.write(require("@vscode/iconv-lite-umd/package.json").version);
  process.exit(0);
}
if (!iconv.encodingExists(encoding)) {
  process.stderr.write(`iconv-lite doesn't know ${encoding}\n`);
  process.exit(2);
}
const lines = fs.readFileSync(0, "utf8").split("\n");
if (lines[lines.length - 1] === "") lines.pop();
let out;
if (command === "decode") {
  out = lines.map((hex) =>
    [...iconv.decode(Buffer.from(hex, "hex"), encoding)]
      .map((c) => c.codePointAt(0).toString(16))
      .join(" "),
  );
} else if (command === "encode") {
  out = lines.map((hex) => {
    const ch = String.fromCodePoint(parseInt(hex, 16));
    const bytes = Buffer.from(iconv.encode(ch, encoding));
    // iconv-lite substitutes "?" for what it can't encode
    return iconv.decode(bytes, encoding) === ch ? bytes.toString("hex") : "-";
  });
} else {
  process.stderr.write(
    "usage: iconv_lite.js version | decode <encoding> | encode <encoding>\n",
  );
  process.exit(2);
}
process.stdout.write(out.join("\n") + "\n");
