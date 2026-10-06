import * as assert from "node:assert";
import * as fs from "node:fs";
import * as path from "node:path";
import { test } from "node:test";
import {
  validateEncodingId,
  isKnownEncoding,
  listEncodingIds,
  ENCODINGS,
  encodingShortName,
  groupForPicker,
  recordRecentEncoding,
} from "../encodings";

function readPackageJson(): {
  activationEvents: readonly string[];
  contributes: {
    terminal: {
      profiles: ReadonlyArray<{ id: string; title: string; icon: string }>;
    };
  };
} {
  const packageJsonPath = path.join(__dirname, "..", "..", "package.json");
  return JSON.parse(fs.readFileSync(packageJsonPath, "utf8"));
}

test("package.json contributes exactly the two dropdown profiles, titled with the 🌐 pseudo-icon", () => {
  // The "New Terminal" ▼ menu renders plain text only and never shows
  // icons, so an emoji is prepended to the title to keep the entries
  // recognizable among other profiles. The titles are also what users put
  // in terminal.integrated.defaultProfile.*, so they're kept stable
  // (not localized).
  assert.deepStrictEqual(readPackageJson().contributes.terminal.profiles, [
    {
      id: "terminalAnyEncoding.select",
      title: "🌐 Select Encoding...",
      icon: "globe",
    },
    {
      id: "terminalAnyEncoding.default",
      title: "🌐 Default Encoding",
      icon: "globe",
    },
  ]);
});

test("package.json's activationEvents cover the commands, profiles and task type (implicit activation needs VS Code 1.74+, engines allows 1.73)", () => {
  assert.deepStrictEqual([...readPackageJson().activationEvents].sort(), [
    "onCommand:terminalAnyEncoding.openDefaultEncodingTerminal",
    "onCommand:terminalAnyEncoding.openTerminal",
    "onTaskType:terminalAnyEncoding",
    "onTerminalProfile:terminalAnyEncoding.default",
    "onTerminalProfile:terminalAnyEncoding.select",
  ]);
});

test("known encodings pass validation", () => {
  for (const e of ENCODINGS) {
    const result = validateEncodingId(e.id);
    assert.strictEqual(result.ok, true);
  }
});

test("unknown encodings are rejected with a reason", () => {
  const result = validateEncodingId("nonexistent-encoding");
  assert.strictEqual(result.ok, false);
  if (!result.ok) {
    // Building the display string (localization) is the vscode-dependent
    // extension.ts's job, so only structured data is verified here.
    assert.strictEqual(result.reason.id, "nonexistent-encoding");
  }
});

test("case differences and spelling variants are rejected as distinct (no lenient matching)", () => {
  assert.strictEqual(isKnownEncoding("EUCJP"), false);
  assert.strictEqual(isKnownEncoding("ShiftJIS"), false);
});

test("UTF-8 isn't offered: it would just open the same terminal as `+`", () => {
  assert.strictEqual(listEncodingIds().includes("utf8"), false);
});

test("Chinese, Korean, and single-byte encodings are included", () => {
  for (const id of [
    "gb18030",
    "gbk",
    "cp950",
    "euckr",
    "iso88591",
    "koi8r",
    "cp852",
  ]) {
    assert.ok(isKnownEncoding(id), `${id} not found`);
  }
});

test("KOI8-T is offered", () => {
  assert.ok(isKnownEncoding("koi8t"));
});

test("ENCODINGS order follows VS Code core's files.encoding (SUPPORTED_ENCODINGS.order)", () => {
  // This is vs/workbench/services/textfile/common/encoding.ts's
  // SUPPORTED_ENCODINGS sorted by ascending order, with the 4 entries this
  // extension doesn't offer (utf8/utf8bom/utf16le/utf16be) removed. To
  // make this look the same order as the files.encoding dropdown, ENCODINGS
  // itself is defined in this order (single source of truth; an existing
  // sync test checks that the package.json side tracks this order too).
  const vscodeOrder = [
    "windows1252",
    "iso88591",
    "iso88593",
    "iso885915",
    "macroman",
    "cp437",
    "windows1256",
    "iso88596",
    "windows1257",
    "iso88594",
    "iso885914",
    "windows1250",
    "iso88592",
    "cp852",
    "windows1251",
    "cp866",
    "cp1125",
    "iso88595",
    "koi8r",
    "koi8u",
    "iso885913",
    "windows1253",
    "iso88597",
    "windows1255",
    "iso88598",
    "iso885910",
    "iso885916",
    "windows1254",
    "iso88599",
    "cp857",
    "windows1258",
    "gbk",
    "gb18030",
    "cp950",
    "big5hkscs",
    "shiftjis",
    "eucjp",
    "euckr",
    "windows874",
    "iso885911",
    "koi8ru",
    "koi8t",
    "gb2312",
    "cp865",
    "cp850",
  ];
  assert.deepStrictEqual(
    ENCODINGS.map((e) => e.id),
    vscodeOrder,
  );
});

test("every encoding's luitEncoding is a non-empty string", () => {
  for (const e of ENCODINGS) {
    assert.ok(e.luitEncoding.length > 0, `${e.id}`);
  }
});

test("package.json's defaultEncoding enum/enumDescriptions/enumItemLabels match ENCODINGS (catches manual-sync drift)", () => {
  const packageJsonPath = path.join(__dirname, "..", "..", "package.json");
  const packageJson = JSON.parse(fs.readFileSync(packageJsonPath, "utf8")) as {
    contributes: {
      configuration: {
        properties: {
          "terminalAnyEncoding.defaultEncoding": {
            enum: readonly string[];
            enumDescriptions: readonly string[];
            enumItemLabels: readonly string[];
          };
        };
      };
    };
  };
  const defaultEncodingProp =
    packageJson.contributes.configuration.properties[
      "terminalAnyEncoding.defaultEncoding"
    ];

  // The first entry is the empty string, meaning "always show the picker".
  assert.strictEqual(defaultEncodingProp.enum[0], "");
  assert.deepStrictEqual(
    defaultEncodingProp.enum.slice(1),
    ENCODINGS.map((e) => e.id),
  );
  assert.deepStrictEqual(
    defaultEncodingProp.enumDescriptions.slice(1),
    ENCODINGS.map((e) => e.label),
  );
  // enumItemLabels is the label shown as the primary text in the Settings
  // UI dropdown (the raw enum value is shown secondarily, alongside it).
  assert.deepStrictEqual(
    defaultEncodingProp.enumItemLabels.slice(1),
    ENCODINGS.map((e) => e.label),
  );
});

test("package.json's task definition offers the same encodings as ENCODINGS (catches manual-sync drift)", () => {
  const packageJsonPath = path.join(__dirname, "..", "..", "package.json");
  const packageJson = JSON.parse(fs.readFileSync(packageJsonPath, "utf8")) as {
    contributes: {
      taskDefinitions: ReadonlyArray<{
        type: string;
        properties: {
          encoding: {
            enum: readonly string[];
            enumDescriptions: readonly string[];
          };
        };
      }>;
    };
  };
  const [definition] = packageJson.contributes.taskDefinitions;
  assert.strictEqual(definition.type, "terminalAnyEncoding");
  assert.deepStrictEqual(
    definition.properties.encoding.enum,
    ENCODINGS.map((e) => e.id),
  );
  assert.deepStrictEqual(
    definition.properties.encoding.enumDescriptions,
    ENCODINGS.map((e) => e.label),
  );
});

test('encodingShortName: the label\'s parenthesized part, for tab names like "bash (EUC-JP)"', () => {
  const byId = (id: string) => ENCODINGS.find((e) => e.id === id)!;
  assert.strictEqual(encodingShortName(byId("eucjp")), "EUC-JP");
  assert.strictEqual(encodingShortName(byId("windows1252")), "Windows 1252");
});

test("recordRecentEncoding: most recent first, no duplicates, unknown ids dropped, at most 3", () => {
  assert.deepStrictEqual(recordRecentEncoding([], "eucjp"), ["eucjp"]);
  assert.deepStrictEqual(
    recordRecentEncoding(["shiftjis", "eucjp", "gbk"], "eucjp"),
    ["eucjp", "shiftjis", "gbk"],
  );
  assert.deepStrictEqual(
    recordRecentEncoding(["shiftjis", "no-such", "gbk", "euckr"], "cp950"),
    ["cp950", "shiftjis", "gbk"],
  );
});

test("groupForPicker: recently used first, then every other encoding in the usual order", () => {
  const { recent, others } = groupForPicker(["shiftjis", "no-such", "eucjp"]);
  assert.deepStrictEqual(
    recent.map((e) => e.id),
    ["shiftjis", "eucjp"],
  );
  assert.deepStrictEqual(
    others.map((e) => e.id),
    ENCODINGS.map((e) => e.id).filter(
      (id) => id !== "shiftjis" && id !== "eucjp",
    ),
  );
  const none = groupForPicker([]);
  assert.deepStrictEqual(none.recent, []);
  assert.strictEqual(none.others.length, ENCODINGS.length);
});
