// @ts-check
import js from "@eslint/js";
import n from "eslint-plugin-n";
import globals from "globals";
import tseslint from "typescript-eslint";

/** eslint-plugin-n's checks against a Node.js version */
const nodeVersion = (version) => ({
  "n/no-unsupported-features/node-builtins": ["error", { version }],
  "n/no-unsupported-features/es-syntax": ["error", { version }],
});

export default tseslint.config(
  { ignores: ["out/**", "**/*.js"] },
  js.configs.recommended,
  tseslint.configs.recommended,
  {
    // Node's globals, declared so eslint-plugin-n can check them (fetch,
    // structuredClone, ...); undeclared ones aren't tracked.
    languageOptions: {
      ecmaVersion: 2022,
      sourceType: "module",
      globals: globals.node,
    },
    plugins: { n },
    rules: {
      "@typescript-eslint/no-unused-vars": "error",
    },
  },
  {
    // The extension itself (runs inside the Extension Host) is guarded, as
    // a safety net, against the Node.js runtime bundled with the lower
    // bound of engines.vscode (1.73.0 -> Node 16.14.2). Only covers global
    // references and module members; prototype methods like Array#at are
    // out of scope, caught separately by tsconfig.json's "lib".
    files: ["src/**/*.ts"],
    ignores: ["src/test/**/*.ts"],
    rules: nodeVersion("16.14.2"),
  },
  {
    // Tests run on the dev/CI machine (the .nvmrc version), so the runtime
    // floor is handled separately.
    files: ["src/test/**/*.ts"],
    rules: nodeVersion(">=20.0.0"),
  },
);
