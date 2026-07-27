// monaco-editor ships type declarations for exactly ONE entry point
// (esm/vs/editor/editor.api.d.ts). The two side-effect entries this bundle also
// pulls in — the standalone editor contributions and the Monarch basic-language
// tokenizers — are plain JavaScript with no .d.ts, so declare them as untyped
// modules. They export nothing we consume; only their registration side effects
// matter, and the typed API surface still comes from editor.api.
declare module "monaco-editor/esm/vs/editor/edcore.main";
declare module "monaco-editor/esm/vs/basic-languages/monaco.contribution";
