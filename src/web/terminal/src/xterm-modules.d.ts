// xterm.js ships its stylesheet as a plain .css file with no type declaration.
// index.ts imports it for its side effect only — esbuild extracts it into
// dist/terminal.css, which the page links — so declare it as an untyped module
// rather than teaching tsc about CSS.
declare module "*.css";
